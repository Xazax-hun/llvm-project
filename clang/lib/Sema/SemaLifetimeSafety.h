//===--- SemaLifetimeSafety.h - Sema support for lifetime safety =---------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines the Sema-specific implementation for lifetime safety
//  analysis. It provides diagnostic reporting and helper functions that bridge
//  the lifetime safety analysis framework with Sema's diagnostic engine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_SEMA_SEMALIFETIMESAFETY_H
#define LLVM_CLANG_LIB_SEMA_SEMALIFETIMESAFETY_H

#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeSafety.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Lex/Lexer.h"
#include "clang/Sema/Sema.h"
#include <string>

namespace clang::lifetimes {

/// Every lifetime-safety diagnostic: the members of `-Wlifetime-safety-all`.
/// Asking about all of them is how "is lifetime safety wanted here?" is
/// answered, since any one of them being enabled requires the analysis to run.
///
/// Derived from the group rather than listed by hand: a hand-written list
/// drifted, and a group whose diagnostics were all missing from it --
/// `-Wlifetime-safety-owner-capture` or `-Wlifetime-safety-openmp` enabled on
/// its own -- never ran the analysis and so reported nothing. The group also
/// holds diagnostics Sema emits at declarations; including them only means the
/// analysis runs when one of those alone is enabled.
///
/// Members enabled by default are left out. Every diagnostic the analysis emits
/// is off by default; a default-on member -- `-Wmissing-noescape`, an upstream
/// Sema warning the soundness group pulls in -- would make the gate true for
/// every function of every compile.
inline ArrayRef<diag::kind> LifetimeSafetyDiagIDs(Sema &S) {
  static const llvm::SmallVector<diag::kind, 0> IDs = [&] {
    const DiagnosticIDs &DiagIDs = *S.getDiagnostics().getDiagnosticIDs();
    llvm::SmallVector<diag::kind, 0> Out;
    [[maybe_unused]] bool Unknown = DiagIDs.getDiagnosticsInGroup(
        diag::Flavor::WarningOrError, "lifetime-safety-all", Out);
    assert(!Unknown && "lifetime-safety-all is not a diagnostic group");
    llvm::erase_if(Out, [&](diag::kind ID) {
      return DiagIDs.getDefaultMapping(ID).getSeverity() !=
             diag::Severity::Ignored;
    });
    return Out;
  }();
  return IDs;
}

/// Whether any lifetime-safety diagnostic is enabled anywhere in \p D.
///
/// Asking only at the start of \p D is not enough: a `#pragma clang diagnostic`
/// ending inside the declaration re-enables the warnings for the rest of its
/// body, and skipping the analysis would then drop every finding there
/// silently. Diagnostics outside the enabled region are still filtered when
/// they are emitted.
inline bool IsLifetimeSafetyEnabled(Sema &S, const Decl *D) {
  if (S.getLangOpts().DebugRunLifetimeSafety)
    return true;
  DiagnosticsEngine &Diags = S.getDiagnostics();
  auto EnabledAt = [&](SourceLocation Loc) {
    for (diag::kind DiagID : LifetimeSafetyDiagIDs(S))
      if (!Diags.isIgnored(DiagID, Loc))
        return true;
    return false;
  };
  if (EnabledAt(D->getBeginLoc()))
    return true;
  return !Diags.forEachStateRegionLocIn(
      D->getSourceRange(), [&](SourceLocation Loc) { return !EnabledAt(Loc); });
}

/// Whether any lifetime-safety diagnostic is enabled ANYWHERE in the
/// translation unit.
///
/// This is the question a whole-translation-unit sweep must ask before it walks
/// the AST. Asking `isIgnored(..., SourceLocation())` instead would consult only
/// the state the command line established, so a `#pragma clang diagnostic`
/// enabling a warning for part of a file would be missed and the sweep skipped
/// -- silently dropping the diagnostics it alone can produce.
inline bool IsLifetimeSafetyEnabledAnywhere(Sema &S) {
  if (S.getLangOpts().DebugRunLifetimeSafety)
    return true;
  DiagnosticsEngine &Diags = S.getDiagnostics();
  for (diag::kind DiagID : LifetimeSafetyDiagIDs(S))
    if (!Diags.isIgnoredInEveryState(DiagID))
      return true;
  return false;
}

class LifetimeSafetySemaHelperImpl : public LifetimeSafetySemaHelper {
  SuppressionCandidatesFn Candidates;

public:
  LifetimeSafetySemaHelperImpl(Sema &S) : S(S) {}

  /// True if the reader has silenced `DiagID` at `L`.
  ///
  /// A lifetime report's primary diagnostic is anchored at the borrow's CREATION:
  /// that is the one location every loan has -- a loan may have many uses -- and
  /// it is what the message names ("parameter is later invalidated"). But a reader
  /// silencing a warning puts the pragma around the code they consider fine, which
  /// is the offending USE:
  ///
  ///   p->~T();
  ///   #pragma clang diagnostic ignored "-Wlifetime-safety-invalidation"
  ///   deallocate(p);            // <- what the reader means
  ///
  /// Clang consults the diagnostic state at the location a diagnostic is emitted
  /// AT, so anchoring at the creation made that pragma do nothing at all. For a
  /// borrow of a PARAMETER it is worse: the creation is the parameter
  /// declaration, so no pragma placed anywhere in the body could suppress it.
  ///
  /// So a report is suppressed when it is silenced at the code it is ABOUT, not
  /// only at the code it points at.
  void setSuppressionCandidates(SuppressionCandidatesFn Compute) override {
    Candidates = Compute;
  }

  bool silencedAt(unsigned DiagID, SourceLocation L) const {
    // The named use first: it is one of the candidates, and when it is not
    // silenced -- nearly always -- nothing else needs computing.
    if (!silencedAtOne(DiagID, L))
      return false;
    // Then every other use of the borrow, when the checker supplies them:
    // suppressing on the named use alone would hide the report for the others.
    // A use with no location cannot have been silenced.
    if (Candidates) {
      llvm::SmallVector<SourceLocation, 8> Locs;
      Candidates(Locs);
      for (SourceLocation C : Locs)
        if (!silencedAtOne(DiagID, C))
          return false;
    }
    return true;
  }

  bool silencedAtOne(unsigned DiagID, SourceLocation L) const {
    // isSilencedAtNoCreate rather than isIgnored: the latter lazily builds the
    // file's diagnostic-state table as a side effect, and this runs mid-parse.
    return L.isValid() && S.getDiagnostics().isSilencedAtNoCreate(DiagID, L);
  }

  void reportUseAfterScope(const Expr *IssueExpr, const Expr *UseExpr,
                           const Expr *MovedExpr,
                           SourceLocation FreeLoc) override {
    unsigned DiagID = MovedExpr
                          ? diag::warn_lifetime_safety_use_after_scope_moved
                          : diag::warn_lifetime_safety_use_after_scope;

    if (silencedAt(DiagID, UseExpr->getExprLoc()))
      return;
    S.Diag(IssueExpr->getExprLoc(), DiagID)
        << getDiagSubjectDescription(IssueExpr) << IssueExpr->getSourceRange();
    if (MovedExpr)
      S.Diag(MovedExpr->getExprLoc(), diag::note_lifetime_safety_moved_here)
          << MovedExpr->getSourceRange();
    S.Diag(FreeLoc, diag::note_lifetime_safety_destroyed_here);
    S.Diag(UseExpr->getExprLoc(), diag::note_lifetime_safety_used_here)
        << UseExpr->getSourceRange();
  }
  void reportUseAfterScope(const Expr *IssueExpr, SourceLocation UseLoc,
                           const Expr *MovedExpr,
                           SourceLocation FreeLoc) override {
    unsigned DiagID = MovedExpr
                          ? diag::warn_lifetime_safety_use_after_scope_moved
                          : diag::warn_lifetime_safety_use_after_scope;
    if (silencedAt(DiagID, UseLoc))
      return;
    S.Diag(IssueExpr->getExprLoc(), DiagID)
        << getDiagSubjectDescription(IssueExpr) << IssueExpr->getSourceRange();
    if (MovedExpr)
      S.Diag(MovedExpr->getExprLoc(), diag::note_lifetime_safety_moved_here)
          << MovedExpr->getSourceRange();
    S.Diag(FreeLoc, diag::note_lifetime_safety_destroyed_here);
    S.Diag(UseLoc, diag::note_lifetime_safety_used_here);
  }

  void reportUseAfterReturn(const Expr *IssueExpr, const Expr *ReturnExpr,
                            const Expr *MovedExpr) override {
    unsigned DiagID = MovedExpr
                          ? diag::warn_lifetime_safety_return_stack_addr_moved
                          : diag::warn_lifetime_safety_return_stack_addr;

    S.Diag(IssueExpr->getExprLoc(), DiagID)
        << getDiagSubjectDescription(IssueExpr) << IssueExpr->getSourceRange();

    if (MovedExpr)
      S.Diag(MovedExpr->getExprLoc(), diag::note_lifetime_safety_moved_here)
          << MovedExpr->getSourceRange();
    S.Diag(ReturnExpr->getExprLoc(), diag::note_lifetime_safety_returned_here)
        << ReturnExpr->getSourceRange();
  }

  void reportDanglingField(const Expr *IssueExpr,
                           const FieldDecl *DanglingField,
                           const Expr *MovedExpr,
                           SourceLocation ExpiryLoc) override {
    unsigned DiagID = MovedExpr
                          ? diag::warn_lifetime_safety_dangling_field_moved
                          : diag::warn_lifetime_safety_dangling_field;

    S.Diag(IssueExpr->getExprLoc(), DiagID)
        << getDiagSubjectDescription(IssueExpr)
        << getDiagSubjectDescription(DanglingField)
        << IssueExpr->getSourceRange();
    if (MovedExpr)
      S.Diag(MovedExpr->getExprLoc(), diag::note_lifetime_safety_moved_here)
          << MovedExpr->getSourceRange();
    S.Diag(DanglingField->getLocation(),
           diag::note_lifetime_safety_dangling_field_here)
        << DanglingField->getEndLoc();
  }

  void reportDanglingGlobal(const Expr *IssueExpr,
                            const VarDecl *DanglingGlobal,
                            const Expr *MovedExpr,
                            SourceLocation ExpiryLoc) override {
    unsigned DiagID = MovedExpr
                          ? diag::warn_lifetime_safety_dangling_global_moved
                          : diag::warn_lifetime_safety_dangling_global;

    S.Diag(IssueExpr->getExprLoc(), DiagID)
        << getDiagSubjectDescription(IssueExpr)
        << getDiagSubjectDescription(DanglingGlobal)
        << IssueExpr->getSourceRange();
    if (MovedExpr)
      S.Diag(MovedExpr->getExprLoc(), diag::note_lifetime_safety_moved_here)
          << MovedExpr->getSourceRange();
    if (DanglingGlobal->isStaticLocal() || DanglingGlobal->isStaticDataMember())
      S.Diag(DanglingGlobal->getLocation(),
             diag::note_lifetime_safety_dangling_static_here)
          << DanglingGlobal->getEndLoc();
    else
      S.Diag(DanglingGlobal->getLocation(),
             diag::note_lifetime_safety_dangling_global_here)
          << DanglingGlobal->getEndLoc();
  }

  void reportUseAfterInvalidation(const Expr *IssueExpr, const Expr *UseExpr,
                                  const Expr *InvalidationExpr) override {
    emitUseAfterInvalidation(anchorAt(IssueExpr), UseExpr->getExprLoc(),
                             UseExpr->getSourceRange(), InvalidationExpr);
  }
  void reportUseAfterInvalidation(const ParmVarDecl *PVD, const Expr *UseExpr,
                                  const Expr *InvalidationExpr) override {
    emitUseAfterInvalidation(anchorAt(PVD), UseExpr->getExprLoc(),
                             UseExpr->getSourceRange(), InvalidationExpr);
  }
  void reportUseAfterInvalidation(const Expr *IssueExpr, SourceLocation UseLoc,
                                  const Expr *InvalidationExpr) override {
    emitUseAfterInvalidation(anchorAt(IssueExpr), UseLoc, SourceRange(),
                             InvalidationExpr);
  }
  void reportUseAfterInvalidation(const ParmVarDecl *PVD, SourceLocation UseLoc,
                                  const Expr *InvalidationExpr) override {
    emitUseAfterInvalidation(anchorAt(PVD), UseLoc, SourceRange(),
                             InvalidationExpr);
  }
  void reportUseAfterInvalidation(const CXXMethodDecl *MD, SourceLocation UseLoc,
                                  const Expr *InvalidationExpr) override {
    // A borrow held by this object: anchored at the method.
    emitUseAfterInvalidation({MD->getBeginLoc(), MD->getSourceRange(), 2},
                             UseLoc, SourceRange(), InvalidationExpr);
  }

  void reportInvalidatedField(const Expr *IssueExpr,
                              const FieldDecl *DanglingField,
                              const Expr *InvalidationExpr) override {
    emitInvalidatedField(anchorAt(IssueExpr), DanglingField, InvalidationExpr);
  }
  void reportInvalidatedField(const ParmVarDecl *PVD,
                              const FieldDecl *DanglingField,
                              const Expr *InvalidationExpr) override {
    emitInvalidatedField(anchorAt(PVD), DanglingField, InvalidationExpr);
  }
  void reportInvalidatedField(const FieldDecl *HoldingField,
                              const FieldDecl *DanglingField,
                              const Expr *InvalidationExpr) override {
    emitInvalidatedField(anchorAt(HoldingField), DanglingField,
                         InvalidationExpr);
  }

  void reportInvalidatedGlobal(const Expr *IssueExpr,
                               const VarDecl *DanglingGlobal,
                               const Expr *InvalidationExpr) override {
    emitInvalidatedGlobal(anchorAt(IssueExpr), DanglingGlobal,
                          InvalidationExpr);
  }
  void reportInvalidatedGlobal(const ParmVarDecl *PVD,
                               const VarDecl *DanglingGlobal,
                               const Expr *InvalidationExpr) override {
    emitInvalidatedGlobal(anchorAt(PVD), DanglingGlobal, InvalidationExpr);
  }
  void reportInvalidatedGlobal(const FieldDecl *HoldingField,
                               const VarDecl *DanglingGlobal,
                               const Expr *InvalidationExpr) override {
    emitInvalidatedGlobal(anchorAt(HoldingField), DanglingGlobal,
                          InvalidationExpr);
  }

  void reportInvalidatedReturn(const Expr *IssueExpr, const Expr *ReturnExpr,
                               const Expr *InvalidationExpr) override {
    emitInvalidatedReturn(anchorAt(IssueExpr), ReturnExpr, InvalidationExpr);
  }
  void reportInvalidatedReturn(const ParmVarDecl *PVD, const Expr *ReturnExpr,
                               const Expr *InvalidationExpr) override {
    emitInvalidatedReturn(anchorAt(PVD), ReturnExpr, InvalidationExpr);
  }
  void reportInvalidatedReturn(const FieldDecl *HoldingField,
                               const Expr *ReturnExpr,
                               const Expr *InvalidationExpr) override {
    emitInvalidatedReturn(anchorAt(HoldingField), ReturnExpr,
                          InvalidationExpr);
  }

  void suggestLifetimeboundToParmVar(WarningScope Scope,
                                     const ParmVarDecl *ParmToAnnotate,
                                     EscapingTarget Target) override {
    unsigned DiagID =
        (Scope == WarningScope::CrossTU)
            ? diag::warn_lifetime_safety_cross_tu_param_suggestion
            : diag::warn_lifetime_safety_intra_tu_param_suggestion;

    auto [InsertionPoint, FixItText] = getLifetimeBoundFixIt(ParmToAnnotate);

    S.Diag(ParmToAnnotate->getBeginLoc(), DiagID)
        << ParmToAnnotate->getSourceRange()
        << FixItHint::CreateInsertion(InsertionPoint, FixItText);

    if (const auto *EscapeExpr = Target.dyn_cast<const Expr *>())
      S.Diag(EscapeExpr->getBeginLoc(),
             diag::note_lifetime_safety_suggestion_returned_here)
          << EscapeExpr->getSourceRange();
    else if (const auto *EscapeField = Target.dyn_cast<const FieldDecl *>())
      S.Diag(EscapeField->getLocation(),
             diag::note_lifetime_safety_escapes_to_field_here)
          << EscapeField->getSourceRange();
  }

  void reportLifetimeboundViolation(
      const ParmVarDecl *ParmWithLifetimebound) override {
    const auto *Attr = ParmWithLifetimebound->getAttr<LifetimeBoundAttr>();
    StringRef ParamName = ParmWithLifetimebound->getName();
    bool HasName = ParamName.size() > 0;
    // On a constructor the annotation describes the constructed object; saying
    // "return value" there is what made this read as nonsense on a constructor,
    // which has none.
    bool IsCtor =
        isa<CXXConstructorDecl>(ParmWithLifetimebound->getDeclContext());
    S.Diag(Attr->getLocation(),
           diag::warn_lifetime_safety_lifetimebound_violation)
        << IsCtor << HasName << ParamName << Attr->getRange();
  }

  void reportLifetimeboundViolation(
      const CXXMethodDecl *MDWithLifetimebound) override {
    const auto *Attr =
        getImplicitObjectParamLifetimeBoundAttr(MDWithLifetimebound);
    assert(Attr && "Expected lifetimebound attribute");
    // '(pointee)' promises a different relationship, so naming the object here
    // would describe a check that was not the one performed.
    unsigned BoundTo = Attr->getBoundTo() == LifetimeBoundAttr::Pointee
                           ? /*what this refers to*/ 3
                           : /*the implicit this parameter*/ 2;
    S.Diag(Attr->getLocation(),
           diag::warn_lifetime_safety_lifetimebound_violation)
        << /*the return value*/ 0 << BoundTo << "" << Attr->getRange();
  }

  void reportCaptureByViolation(const ParmVarDecl *PVD) override {
    const auto *Attr = PVD->getAttr<LifetimeCaptureByAttr>();
    SourceLocation Loc = Attr ? Attr->getLocation() : PVD->getLocation();
    SourceRange Range = Attr ? Attr->getRange() : PVD->getSourceRange();
    StringRef ParamName = PVD->getName();
    bool HasName = ParamName.size() > 0;
    S.Diag(Loc, diag::warn_lifetime_safety_capture_by_violation)
        << HasName << ParamName << Range;
  }

  void reportUndeclaredFieldCapture(const ParmVarDecl *PVD,
                                    const ParmVarDecl *Capturer) override {
    const auto *Attr = PVD->getAttr<LifetimeBoundAttr>();
    SourceLocation Loc = Attr ? Attr->getLocation() : PVD->getLocation();
    SourceRange Range = Attr ? Attr->getRange() : PVD->getSourceRange();
    StringRef ParamName = PVD->getName();
    bool HasName = ParamName.size() > 0;
    S.Diag(Loc, diag::warn_lifetime_safety_undeclared_field_capture)
        << HasName << ParamName << (Capturer != nullptr)
        << (Capturer ? Capturer->getName() : StringRef()) << Range;
  }

  void reportCaptureIntoBorrowlessObject(const Expr *E) override {
    S.Diag(E->getExprLoc(),
           diag::warn_lifetime_safety_capture_into_borrowless_object)
        << E->getSourceRange();
  }
  void reportOwnershipTakesThisViolation(const FunctionDecl *FD) override {
    S.Diag(FD->getLocation(),
           diag::warn_lifetime_safety_ownership_takes_this_violation)
        << FD->getSourceRange();
  }

  void reportMallocViolation(const FunctionDecl *FD,
                            unsigned Subject) override {
    const auto *Attr = FD->getAttr<RestrictAttr>();
    SourceLocation Loc = Attr ? Attr->getLocation() : FD->getLocation();
    S.Diag(Loc, diag::warn_lifetime_safety_malloc_violation) << Subject;
  }
  void reportImmortalViolation(const FunctionDecl *FD,
                               unsigned Subject) override {
    const auto *Attr = FD->getAttr<LifetimeImmortalAttr>();
    SourceLocation Loc = Attr ? Attr->getLocation() : FD->getLocation();
    S.Diag(Loc, diag::warn_lifetime_safety_immortal_violation) << Subject;
  }

  void reportNonInvalidatingParamViolation(const ParmVarDecl *PVD,
                                           SourceLocation Loc) override {
    // Anchor at the attribute: that is the claim being contradicted.
    const auto *Attr = PVD->getAttr<LifetimeNonInvalidatingAttr>();
    S.Diag(Attr ? Attr->getLocation() : PVD->getLocation(),
           diag::warn_lifetime_safety_non_invalidating_param_violation)
        << PVD;
    if (Loc.isValid())
      S.Diag(Loc, diag::note_lifetime_safety_invalidated_here);
  }

  void reportNonInvalidatingViolation(const CXXMethodDecl *MD,
                                      const ValueDecl *Input,
                                      SourceLocation Loc) override {
    // Anchor at the attribute: that is the claim being contradicted, and it is
    // where the author must act. Point at the invalidating operation too.
    const auto *Attr = MD->getAttr<LifetimeNonInvalidatingAttr>();
    SourceLocation AttrLoc = Attr ? Attr->getLocation() : MD->getLocation();
    unsigned Subject = !Input ? 0 : (isa<FieldDecl>(Input) ? 2 : 1);
    S.Diag(AttrLoc, diag::warn_lifetime_safety_non_invalidating_violation)
        << Subject << Input;
    if (Loc.isValid())
      S.Diag(Loc, diag::note_lifetime_safety_invalidated_here);
  }

  void reportMisplacedLifetimebound(WarningScope Scope,
                                    const CXXMethodDecl *FDef,
                                    const CXXMethodDecl *FDecl) override {
    const auto *Attr = getDirectImplicitObjectLifetimeBoundAttr(FDef);
    assert(Attr && "Expected lifetimebound attribute");
    unsigned DiagID =
        Scope == WarningScope::CrossTU
            ? diag::warn_lifetime_safety_cross_tu_misplaced_lifetimebound
            : diag::warn_lifetime_safety_intra_tu_misplaced_lifetimebound;

    auto [InsertionPoint, FixItText] = getLifetimeBoundFixIt(FDecl);

    // Do not emit fix-its in macros or at invalid locations.
    bool IsMacro =
        FDecl->getBeginLoc().isMacroID() || InsertionPoint.isMacroID();

    if (IsMacro || InsertionPoint.isInvalid())
      S.Diag(FDecl->getLocation(), DiagID);
    else
      S.Diag(InsertionPoint, DiagID)
          << FixItHint::CreateInsertion(InsertionPoint, FixItText);

    S.Diag(Attr->getLocation(), diag::note_lifetime_safety_lifetimebound_here)
        << Attr->getRange();
  }

  void reportMisplacedLifetimebound(WarningScope Scope,
                                    const ParmVarDecl *PVDDef,
                                    const ParmVarDecl *PVDDecl) override {

    const auto *Attr = PVDDef->getAttr<LifetimeBoundAttr>();
    assert(Attr && "Expected lifetimebound attribute");
    unsigned DiagID =
        Scope == WarningScope::CrossTU
            ? diag::warn_lifetime_safety_cross_tu_misplaced_lifetimebound
            : diag::warn_lifetime_safety_intra_tu_misplaced_lifetimebound;

    auto [InsertionPoint, FixItText] = getLifetimeBoundFixIt(PVDDecl);

    // Do not emit fix-its in macros or at invalid locations.
    bool IsMacro =
        PVDDecl->getBeginLoc().isMacroID() || InsertionPoint.isMacroID();

    if (IsMacro || InsertionPoint.isInvalid())
      S.Diag(PVDDecl->getBeginLoc(), DiagID) << PVDDecl->getSourceRange();
    else
      S.Diag(PVDDecl->getBeginLoc(), DiagID)
          << PVDDecl->getSourceRange()
          << FixItHint::CreateInsertion(InsertionPoint, FixItText);

    S.Diag(Attr->getLocation(), diag::note_lifetime_safety_lifetimebound_here)
        << Attr->getRange();
  }

  void suggestLifetimeboundToImplicitThis(WarningScope Scope,
                                          const CXXMethodDecl *MD,
                                          const Expr *EscapeExpr) override {
    unsigned DiagID = (Scope == WarningScope::CrossTU)
                          ? diag::warn_lifetime_safety_cross_tu_this_suggestion
                          : diag::warn_lifetime_safety_intra_tu_this_suggestion;

    auto [InsertionPoint, FixItText] = getLifetimeBoundFixIt(MD);

    S.Diag(InsertionPoint, DiagID)
        << MD->getNameInfo().getSourceRange()
        << FixItHint::CreateInsertion(InsertionPoint, FixItText);

    S.Diag(EscapeExpr->getBeginLoc(),
           diag::note_lifetime_safety_suggestion_returned_here)
        << EscapeExpr->getSourceRange();
  }

  void reportNoescapeViolation(const ParmVarDecl *ParmWithNoescape,
                               const Expr *EscapeExpr) override {
    S.Diag(ParmWithNoescape->getBeginLoc(),
           diag::warn_lifetime_safety_noescape_escapes)
        << ParmWithNoescape->getSourceRange();

    S.Diag(EscapeExpr->getBeginLoc(),
           diag::note_lifetime_safety_suggestion_returned_here)
        << EscapeExpr->getSourceRange();
  }

  void reportLocalEscapesIntoCallerObject(const ValueDecl *Local,
                                          const Expr *StoreExpr) override {
    S.Diag(StoreExpr->getExprLoc(),
           diag::warn_lifetime_safety_dangling_caller_object)
        << Local << StoreExpr->getSourceRange();
  }

  void reportNoescapeStoreIntoParam(const ParmVarDecl *ParmWithNoescape,
                                    const Expr *StoreExpr) override {
    S.Diag(ParmWithNoescape->getBeginLoc(),
           diag::warn_lifetime_safety_noescape_escapes)
        << ParmWithNoescape->getSourceRange();

    S.Diag(StoreExpr->getBeginLoc(),
           diag::note_lifetime_safety_escapes_into_param_here)
        << StoreExpr->getSourceRange();
  }

  void reportNoescapeViolation(const ParmVarDecl *ParmWithNoescape,
                               const FieldDecl *EscapeField) override {
    S.Diag(ParmWithNoescape->getBeginLoc(),
           diag::warn_lifetime_safety_noescape_escapes)
        << ParmWithNoescape->getSourceRange();

    S.Diag(EscapeField->getLocation(),
           diag::note_lifetime_safety_escapes_to_field_here)
        << EscapeField->getEndLoc();
  }

  void reportNoescapeViolation(const ParmVarDecl *ParmWithNoescape,
                               const VarDecl *EscapeGlobal) override {
    S.Diag(ParmWithNoescape->getBeginLoc(),
           diag::warn_lifetime_safety_noescape_escapes)
        << ParmWithNoescape->getSourceRange();
    if (EscapeGlobal->isStaticLocal() || EscapeGlobal->isStaticDataMember())
      S.Diag(EscapeGlobal->getLocation(),
             diag::note_lifetime_safety_escapes_to_static_storage_here)
          << EscapeGlobal->getEndLoc();
    else
      S.Diag(EscapeGlobal->getLocation(),
             diag::note_lifetime_safety_escapes_to_global_here)
          << EscapeGlobal->getEndLoc();
  }

  void addLifetimeBoundToImplicitThis(const CXXMethodDecl *MD) override {
    S.addLifetimeBoundToImplicitThis(const_cast<CXXMethodDecl *>(MD));
  }

  void reportLostLoan(const Expr *UseExpr) override {
    std::string Subject = getDiagSubjectDescription(UseExpr);
    if (Subject.empty())
      Subject = "this value";
    S.Diag(UseExpr->getExprLoc(), diag::warn_lifetime_safety_lost_loan)
        << Subject << UseExpr->getSourceRange();
  }

  void reportAnalysisBailout(const Decl *FD, BailoutReason Reason) override {
    if (!FD)
      return;
    S.Diag(FD->getLocation(), diag::warn_lifetime_safety_bailout)
        << static_cast<unsigned>(Reason);
  }

  void reportDefaultArgTemporary(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_default_arg_temporary)
        << E->getSourceRange();
  }

  void reportIndirectCall(const Expr *CallExpr) override {
    S.Diag(CallExpr->getExprLoc(), diag::warn_lifetime_safety_indirect_call)
        << CallExpr->getSourceRange();
  }

  void reportUnannotatedIndirection(const Expr *ArgExpr) override {
    S.Diag(ArgExpr->getExprLoc(),
           diag::warn_lifetime_safety_unannotated_indirection)
        << ArgExpr->getSourceRange();
  }

  void reportUnannotatedParam(const ParmVarDecl *PVD) override {
    S.Diag(PVD->getBeginLoc(), diag::warn_lifetime_safety_unannotated_param)
        << PVD->getSourceRange();
  }

  void reportUnannotatedThisReturn(const CXXMethodDecl *MD,
                                   unsigned Which) override {
    S.Diag(MD->getLocation(),
           diag::warn_lifetime_safety_unannotated_this_return)
        << MD->getReturnType() << Which << MD->getSourceRange();
  }

  void reportThisEscapesToGlobal(SourceLocation Loc, bool IsField,
                                 const VarDecl *Global) override {
    S.Diag(Loc, diag::warn_lifetime_safety_this_escapes_to_global) << Global;
  }

  void reportAnnotatedParamEscapesToGlobal(const ParmVarDecl *PVD,
                                           const VarDecl *Global) override {
    S.Diag(PVD->getLocation(),
           diag::warn_lifetime_safety_annotated_param_escapes_to_global)
        << Global << PVD->getSourceRange();
  }

  void reportMultiLevelIndirection(const ValueDecl *VD) override {
    S.Diag(VD->getLocation(),
           diag::warn_lifetime_safety_multilevel_indirection)
        << getDiagSubjectDescription(VD) << VD->getSourceRange();
  }
  void reportMultiLevelIndirection(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_multilevel_indirection)
        << getDiagSubjectDescription(E) << E->getSourceRange();
  }
  void reportMultiLevelIndirectionReturn(const FunctionDecl *FD) override {
    std::string Subject = "the return type of '";
    llvm::raw_string_ostream OS(Subject);
    FD->getNameForDiagnostic(OS, S.getPrintingPolicy(), /*Qualified=*/false);
    OS << "'";
    SourceRange RTR = FD->getReturnTypeSourceRange();
    S.Diag(RTR.isValid() ? RTR.getBegin() : FD->getLocation(),
           diag::warn_lifetime_safety_multilevel_indirection)
        << Subject << RTR;
  }
  void reportMultiLevelIndirectionCapture(const Expr *CaptureRef) override {
    std::string Subject = "by-reference capture";
    if (const auto *DRE =
            dyn_cast<DeclRefExpr>(CaptureRef->IgnoreParenImpCasts())) {
      Subject += " of '";
      llvm::raw_string_ostream OS(Subject);
      DRE->getDecl()->getNameForDiagnostic(OS, S.getPrintingPolicy(),
                                           /*Qualified=*/false);
      OS << "'";
    }
    S.Diag(CaptureRef->getExprLoc(),
           diag::warn_lifetime_safety_multilevel_indirection)
        << Subject << CaptureRef->getSourceRange();
  }
  void reportArrayOfIndirectionDecay(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_multilevel_indirection)
        << "this array of pointers (decaying to a pointer to a pointer)"
        << E->getSourceRange();
  }
  void reportAssignmentDereferencesMember(const Expr *E,
                                          SourceLocation Loc) override {
    auto D = S.Diag(
        Loc, diag::warn_lifetime_safety_assignment_dereferences_member);
    if (E)
      D << E->getSourceRange();
  }
  void reportBinaryConditionalTemporary(const Expr *E) override {
    S.Diag(E->getExprLoc(),
           diag::warn_lifetime_safety_binary_conditional_temporary)
        << E->getSourceRange();
  }
  void reportUnsupportedStoreDestination(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_unsupported_store)
        << E->getSourceRange();
  }
  void reportLambdaStoreIntoCapturedObject(const Expr *E,
                                           SourceLocation Loc) override {
    auto D = S.Diag(Loc,
                    diag::warn_lifetime_safety_lambda_store_into_captured_object);
    if (E)
      D << E->getSourceRange();
  }
  void reportUnmodeledExpr(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_unmodeled_expr)
        << E->getSourceRange();
  }

  void reportMoveSilencing(const Expr *MoveExpr) override {
    S.Diag(MoveExpr->getExprLoc(), diag::warn_lifetime_safety_move_silencing)
        << MoveExpr->getSourceRange();
  }

  void reportAssumedInvalidation(const Expr *IssueExpr,
                                 const Stmt *OperationStmt) override {
    S.Diag(IssueExpr->getExprLoc(),
           diag::warn_lifetime_safety_assumed_invalidation)
        << /*object=*/0 << IssueExpr->getSourceRange();
    S.Diag(OperationStmt->getBeginLoc(),
           diag::note_lifetime_safety_assumed_invalidated_here)
        << OperationStmt->getSourceRange();
  }
  void reportAssumedInvalidation(const ParmVarDecl *PVD,
                                 const Stmt *OperationStmt) override {
    S.Diag(PVD->getBeginLoc(), diag::warn_lifetime_safety_assumed_invalidation)
        << /*parameter=*/1 << PVD->getSourceRange();
    S.Diag(OperationStmt->getBeginLoc(),
           diag::note_lifetime_safety_assumed_invalidated_here)
        << OperationStmt->getSourceRange();
  }
  void reportAssumedInvalidation(const CXXMethodDecl *MD,
                                 const Stmt *OperationStmt) override {
    S.Diag(MD->getBeginLoc(), diag::warn_lifetime_safety_assumed_invalidation)
        << /*implicit object parameter=*/2 << MD->getSourceRange();
    S.Diag(OperationStmt->getBeginLoc(),
           diag::note_lifetime_safety_assumed_invalidated_here)
        << OperationStmt->getSourceRange();
  }

  void reportNakedDeallocation(const Expr *DeallocExpr) override {
    S.Diag(DeallocExpr->getExprLoc(),
           diag::warn_lifetime_safety_naked_delete)
        << (isa<CXXDeleteExpr>(DeallocExpr) ? /*deleting=*/0 : /*freeing=*/1)
        << DeallocExpr->getSourceRange();
  }

  void reportUnknownOwnership(const ValueDecl *VD) override {
    S.Diag(VD->getLocation(), diag::warn_lifetime_safety_unknown_ownership)
        << VD->getType() << VD->getSourceRange();
  }
  void reportUnknownOwnership(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_unknown_ownership)
        << E->getType() << E->getSourceRange();
  }
  void reportException(SourceLocation Loc) override {
    S.Diag(Loc, diag::warn_lifetime_safety_exception);
  }
  void reportInlineAsm(SourceLocation Loc) override {
    S.Diag(Loc, diag::warn_lifetime_safety_inline_asm);
  }
  void reportOpenMPDirective(SourceLocation Loc) override {
    S.Diag(Loc, diag::warn_lifetime_safety_openmp) << 0;
  }
  void reportSetjmpLongjmp(SourceLocation Loc) override {
    S.Diag(Loc, diag::warn_lifetime_safety_setjmp);
  }
  void reportCoroutine(SourceLocation Loc) override {
    S.Diag(Loc, diag::warn_lifetime_safety_coroutine);
  }
  void reportUnion(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_union)
        << E->getSourceRange();
  }
  void reportReinterpretCast(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_reinterpret_cast)
        << E->getSourceRange();
  }
  void reportVoidPointerCast(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_void_pointer_cast)
        << E->getSourceRange();
  }
  void reportDowncast(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_downcast)
        << E->getSourceRange();
  }
  void reportOwnerOfIndirection(const ValueDecl *VD,
                                QualType ReportType) override {
    S.Diag(VD->getLocation(), diag::warn_lifetime_safety_owner_of_indirection)
        << (ReportType.isNull() ? VD->getType() : ReportType)
        << VD->getSourceRange();
  }
  void reportOwnerOfIndirection(const Expr *E, QualType ReportType) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_owner_of_indirection)
        << (ReportType.isNull() ? E->getType() : ReportType)
        << E->getSourceRange();
  }
  void reportPointerOfIndirection(const ValueDecl *VD,
                                  QualType ReportType) override {
    S.Diag(VD->getLocation(),
           diag::warn_lifetime_safety_pointer_of_indirection)
        << (ReportType.isNull() ? VD->getType() : ReportType)
        << VD->getSourceRange();
  }
  void reportPointerOfIndirection(const Expr *E, QualType ReportType) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_pointer_of_indirection)
        << (ReportType.isNull() ? E->getType() : ReportType)
        << E->getSourceRange();
  }
  void reportViewOnMutableGlobal(SourceLocation Loc, QualType ViewTy,
                                 SourceRange Range) override {
    S.Diag(Loc, diag::warn_lifetime_safety_view_on_mutable_global)
        << ViewTy << Range;
  }
  void reportGlobalDtorOrder(SourceLocation Loc, QualType ViewTy,
                             SourceRange Range,
                             GlobalDtorOrderRoute Route) override {
    S.Diag(Loc, diag::warn_lifetime_safety_global_dtor_order)
        << ViewTy << unsigned(Route) << Range;
  }
  void reportConstMethodIndirectMutation(const Expr *E) override {
    S.Diag(E->getExprLoc(),
           diag::warn_lifetime_safety_const_indirect_mutation)
        << E->getSourceRange();
  }
  void reportConstMethodIndirectEscape(const Expr *E) override {
    S.Diag(E->getExprLoc(),
           diag::warn_lifetime_safety_const_indirect_escape)
        << E->getSourceRange();
  }
  void reportSelfReferentialBorrow(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_self_referential)
        << E->getType() << E->getSourceRange();
  }
  void reportBaseBorrowsMember(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_base_borrows_member)
        << E->getType() << E->getSourceRange();
  }

private:
  std::pair<SourceLocation, StringRef>
  getLifetimeBoundFixIt(const ParmVarDecl *Decl) {
    SourceLocation InsertionPoint = Lexer::getLocForEndOfToken(
        Decl->getEndLoc(), 0, S.getSourceManager(), S.getLangOpts());
    StringRef FixItText = " [[clang::lifetimebound]]";

    if (!Decl->getIdentifier()) {
      // For unnamed parameters, placing attributes after the type would be
      // parsed as a type attribute, not a parameter attribute.
      InsertionPoint = Decl->getBeginLoc();
      FixItText = "[[clang::lifetimebound]] ";
    } else if (Decl->hasDefaultArg()) {
      // If the parameter has a default argument, place the attribute after the
      // named argument.
      InsertionPoint = Lexer::getLocForEndOfToken(
          Decl->getLocation(), 0, S.getSourceManager(), S.getLangOpts());
    }
    return {InsertionPoint, FixItText};
  }

  std::pair<SourceLocation, StringRef>
  getLifetimeBoundFixIt(const CXXMethodDecl *MD) {
    const auto MDL = MD->getTypeSourceInfo()->getTypeLoc();
    SourceLocation InsertionPoint = Lexer::getLocForEndOfToken(
        MDL.getEndLoc(), 0, S.getSourceManager(), S.getLangOpts());

    if (const auto *FPT = MD->getType()->getAs<FunctionProtoType>();
        FPT && FPT->hasTrailingReturn()) {
      // For trailing return types, 'getEndLoc()' includes the return type
      // after '->', placing the attribute in an invalid position.
      // Instead use 'getLocalRangeEnd()' which gives the '->' location
      // for trailing returns, so find the last token before it.
      const auto FTL = MDL.getAs<FunctionTypeLoc>();
      assert(FTL);
      InsertionPoint = Lexer::getLocForEndOfToken(
          Lexer::findPreviousToken(FTL.getLocalRangeEnd(), S.getSourceManager(),
                                   S.getLangOpts(),
                                   /*IncludeComments=*/false)
              ->getLocation(),
          0, S.getSourceManager(), S.getLangOpts());
    }
    return {InsertionPoint, " [[clang::lifetimebound]]"};
  }

  /// What an invalidation report is anchored at: the borrow, as the location
  /// and range of the primary diagnostic and the `%select` index naming it --
  /// 0 a borrow taken by an expression, 1 a parameter, 2 a member (or, for a
  /// use-after-invalidation, a borrow held by this object).
  struct InvalidationAnchor {
    SourceLocation Loc;
    SourceRange Range;
    unsigned Subject;
  };
  static InvalidationAnchor anchorAt(const Expr *IssueExpr) {
    return {IssueExpr->getExprLoc(), IssueExpr->getSourceRange(), 0};
  }
  static InvalidationAnchor anchorAt(const ParmVarDecl *PVD) {
    return {PVD->getSourceRange().getBegin(), PVD->getSourceRange(), 1};
  }
  static InvalidationAnchor anchorAt(const FieldDecl *HoldingField) {
    return {HoldingField->getLocation(), HoldingField->getSourceRange(), 2};
  }

  /// The note naming the operation: a `delete` frees, anything else
  /// invalidates.
  void noteInvalidation(const Expr *InvalidationExpr) {
    S.Diag(InvalidationExpr->getExprLoc(),
           isa<CXXDeleteExpr>(InvalidationExpr)
               ? diag::note_lifetime_safety_freed_here
               : diag::note_lifetime_safety_invalidated_here)
        << InvalidationExpr->getSourceRange();
  }

  /// Shared body of the reportUseAfterInvalidation overloads. `UseRange` is
  /// empty for an implicit use, which has no expression.
  void emitUseAfterInvalidation(InvalidationAnchor A, SourceLocation UseLoc,
                                SourceRange UseRange,
                                const Expr *InvalidationExpr) {
    unsigned WarnDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                            ? diag::warn_lifetime_safety_use_after_free
                            : diag::warn_lifetime_safety_invalidation;
    if (silencedAt(WarnDiag, UseLoc))
      return;
    S.Diag(A.Loc, WarnDiag) << A.Subject << A.Range;
    noteInvalidation(InvalidationExpr);
    S.Diag(UseLoc, diag::note_lifetime_safety_used_here) << UseRange;
  }

  /// Shared body of the reportInvalidatedField overloads: the invalidated
  /// borrow rests in a member when the function returns.
  void emitInvalidatedField(InvalidationAnchor A, const FieldDecl *DanglingField,
                            const Expr *InvalidationExpr) {
    S.Diag(A.Loc, diag::warn_lifetime_safety_invalidated_field)
        << A.Subject << A.Range;
    noteInvalidation(InvalidationExpr);
    S.Diag(DanglingField->getLocation(),
           diag::note_lifetime_safety_dangling_field_here)
        << DanglingField->getEndLoc();
  }

  /// Shared body of the reportInvalidatedGlobal overloads: the invalidated
  /// borrow rests in global or static storage.
  void emitInvalidatedGlobal(InvalidationAnchor A, const VarDecl *DanglingGlobal,
                             const Expr *InvalidationExpr) {
    S.Diag(A.Loc, diag::warn_lifetime_safety_invalidated_global)
        << A.Subject << A.Range;
    noteInvalidation(InvalidationExpr);
    S.Diag(DanglingGlobal->getLocation(),
           DanglingGlobal->isStaticLocal() || DanglingGlobal->isStaticDataMember()
               ? diag::note_lifetime_safety_dangling_static_here
               : diag::note_lifetime_safety_dangling_global_here)
        << DanglingGlobal->getEndLoc();
  }

  /// Shared body of the reportInvalidatedReturn overloads: the borrow was
  /// invalidated and then handed to the caller, so the report names the borrow,
  /// the operation that invalidated it, and the return that leaks it.
  void emitInvalidatedReturn(InvalidationAnchor A, const Expr *ReturnExpr,
                             const Expr *InvalidationExpr) {
    S.Diag(A.Loc, diag::warn_lifetime_safety_invalidated_return)
        << A.Subject << A.Range;
    noteInvalidation(InvalidationExpr);
    if (ReturnExpr)
      S.Diag(ReturnExpr->getExprLoc(), diag::note_lifetime_safety_returned_here)
          << ReturnExpr->getSourceRange();
  }

  std::string getDiagSubjectDescription(const ValueDecl *VD) {
    std::string Res;
    llvm::raw_string_ostream OS(Res);
    if (isa<FieldDecl>(VD)) {
      OS << "field";
    } else if (isa<ParmVarDecl>(VD)) {
      OS << "parameter";
    } else if (const auto *Var = dyn_cast<VarDecl>(VD)) {
      if (Var->isStaticLocal() || Var->isStaticDataMember())
        OS << "static variable";
      else if (Var->hasGlobalStorage())
        OS << "global variable";
      else
        OS << "local variable";
    } else {
      OS << "variable";
    }
    OS << " '";
    VD->getNameForDiagnostic(OS, S.getPrintingPolicy(), /*Qualified=*/false);
    OS << "'";
    return Res;
  }

  std::string getDiagSubjectDescription(const Expr *E) {
    if (isa<MaterializeTemporaryExpr>(E))
      return "local temporary object";

    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return getDiagSubjectDescription(DRE->getDecl());
    // A static data member reached through an object (`r.slot`) names the same
    // variable as the qualified spelling (`R::slot`); describe it the same way
    // rather than falling back to the generic wording.
    if (const auto *ME = dyn_cast<MemberExpr>(E))
      if (const auto *Var = dyn_cast<VarDecl>(ME->getMemberDecl());
          Var && Var->hasGlobalStorage())
        return getDiagSubjectDescription(Var);
    // TODO: Handle other expression types.
    return "";
  }

  Sema &S;
};

} // namespace clang::lifetimes

#endif // LLVM_CLANG_LIB_SEMA_SEMALIFETIMESAFETY_H
