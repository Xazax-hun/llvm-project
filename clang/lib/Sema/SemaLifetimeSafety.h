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

inline bool IsLifetimeSafetyEnabled(Sema &S, const Decl *D) {
  if (S.getLangOpts().DebugRunLifetimeSafety)
    return true;
  DiagnosticsEngine &Diags = S.getDiagnostics();
  constexpr unsigned DiagIDs[] = {
      diag::warn_lifetime_safety_use_after_scope,
      diag::warn_lifetime_safety_use_after_scope_moved,
      diag::warn_lifetime_safety_use_after_free,
      diag::warn_lifetime_safety_return_stack_addr,
      diag::warn_lifetime_safety_return_stack_addr_moved,
      diag::warn_lifetime_safety_invalidation,
      diag::warn_lifetime_safety_dangling_field,
      diag::warn_lifetime_safety_dangling_field_moved,
      diag::warn_lifetime_safety_dangling_global,
      diag::warn_lifetime_safety_dangling_global_moved,
      diag::warn_lifetime_safety_noescape_escapes,
      diag::warn_lifetime_safety_lifetimebound_violation,
      diag::warn_lifetime_safety_capture_by_violation,
      diag::warn_lifetime_safety_immortal_violation,
      diag::warn_lifetime_safety_cross_tu_misplaced_lifetimebound,
      diag::warn_lifetime_safety_intra_tu_misplaced_lifetimebound,
      diag::warn_lifetime_safety_invalidated_field,
      diag::warn_lifetime_safety_invalidated_global,
      diag::warn_lifetime_safety_cross_tu_param_suggestion,
      diag::warn_lifetime_safety_intra_tu_param_suggestion,
      diag::warn_lifetime_safety_cross_tu_this_suggestion,
      diag::warn_lifetime_safety_intra_tu_this_suggestion,
      diag::warn_lifetime_safety_lost_loan,
      diag::warn_lifetime_safety_bailout,
      diag::warn_lifetime_safety_indirect_call,
      diag::warn_lifetime_safety_unannotated_indirection,
      diag::warn_lifetime_safety_unannotated_param,
      diag::warn_lifetime_safety_unannotated_this_return,
      diag::warn_lifetime_safety_this_escapes_to_global,
      diag::warn_lifetime_safety_annotated_param_escapes_to_global,
      diag::warn_lifetime_safety_global_capture,
      diag::warn_lifetime_safety_multilevel_indirection,
      diag::warn_lifetime_safety_move_silencing,
      diag::warn_lifetime_safety_assumed_invalidation,
      diag::warn_lifetime_safety_naked_delete,
      diag::warn_lifetime_safety_unknown_ownership,
      diag::warn_lifetime_safety_exception,
      diag::warn_lifetime_safety_inline_asm,
      diag::warn_lifetime_safety_setjmp,
      diag::warn_lifetime_safety_coroutine,
      diag::warn_lifetime_safety_union,
      diag::warn_lifetime_safety_reinterpret_cast,
      diag::warn_lifetime_safety_owner_of_indirection,
      diag::warn_lifetime_safety_pointer_of_indirection,
      diag::warn_lifetime_safety_view_on_mutable_global,
      diag::warn_lifetime_safety_const_indirect_mutation,
      diag::warn_lifetime_safety_const_indirect_escape,
      diag::warn_lifetime_safety_self_referential,
      diag::warn_lifetime_safety_unsupported_store,
      diag::warn_lifetime_safety_unmodeled_expr};
  for (unsigned DiagID : DiagIDs)
    if (!Diags.isIgnored(DiagID, D->getBeginLoc()))
      return true;
  return false;
}

class LifetimeSafetySemaHelperImpl : public LifetimeSafetySemaHelper {

public:
  LifetimeSafetySemaHelperImpl(Sema &S) : S(S) {}

  void reportUseAfterScope(const Expr *IssueExpr, const Expr *UseExpr,
                           const Expr *MovedExpr,
                           SourceLocation FreeLoc) override {
    unsigned DiagID = MovedExpr
                          ? diag::warn_lifetime_safety_use_after_scope_moved
                          : diag::warn_lifetime_safety_use_after_scope;

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
    auto WarnDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                        ? diag::warn_lifetime_safety_use_after_free
                        : diag::warn_lifetime_safety_invalidation;
    auto UseDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                       ? diag::note_lifetime_safety_freed_here
                       : diag::note_lifetime_safety_invalidated_here;
    S.Diag(IssueExpr->getExprLoc(), WarnDiag)
        << false << IssueExpr->getSourceRange();
    S.Diag(InvalidationExpr->getExprLoc(), UseDiag)
        << InvalidationExpr->getSourceRange();
    S.Diag(UseExpr->getExprLoc(), diag::note_lifetime_safety_used_here)
        << UseExpr->getSourceRange();
  }
  void reportUseAfterInvalidation(const ParmVarDecl *PVD, const Expr *UseExpr,
                                  const Expr *InvalidationExpr) override {

    auto WarnDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                        ? diag::warn_lifetime_safety_use_after_free
                        : diag::warn_lifetime_safety_invalidation;
    auto UseDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                       ? diag::note_lifetime_safety_freed_here
                       : diag::note_lifetime_safety_invalidated_here;

    S.Diag(PVD->getSourceRange().getBegin(), WarnDiag)
        << true << PVD->getSourceRange();
    S.Diag(InvalidationExpr->getExprLoc(), UseDiag)
        << InvalidationExpr->getSourceRange();
    S.Diag(UseExpr->getExprLoc(), diag::note_lifetime_safety_used_here)
        << UseExpr->getSourceRange();
  }
  void reportUseAfterInvalidation(const Expr *IssueExpr, SourceLocation UseLoc,
                                  const Expr *InvalidationExpr) override {
    auto WarnDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                        ? diag::warn_lifetime_safety_use_after_free
                        : diag::warn_lifetime_safety_invalidation;
    auto UseDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                       ? diag::note_lifetime_safety_freed_here
                       : diag::note_lifetime_safety_invalidated_here;
    S.Diag(IssueExpr->getExprLoc(), WarnDiag)
        << false << IssueExpr->getSourceRange();
    S.Diag(InvalidationExpr->getExprLoc(), UseDiag)
        << InvalidationExpr->getSourceRange();
    S.Diag(UseLoc, diag::note_lifetime_safety_used_here);
  }
  void reportUseAfterInvalidation(const ParmVarDecl *PVD, SourceLocation UseLoc,
                                  const Expr *InvalidationExpr) override {
    auto WarnDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                        ? diag::warn_lifetime_safety_use_after_free
                        : diag::warn_lifetime_safety_invalidation;
    auto UseDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                       ? diag::note_lifetime_safety_freed_here
                       : diag::note_lifetime_safety_invalidated_here;
    S.Diag(PVD->getSourceRange().getBegin(), WarnDiag)
        << true << PVD->getSourceRange();
    S.Diag(InvalidationExpr->getExprLoc(), UseDiag)
        << InvalidationExpr->getSourceRange();
    S.Diag(UseLoc, diag::note_lifetime_safety_used_here);
  }

  void reportInvalidatedField(const Expr *IssueExpr,
                              const FieldDecl *DanglingField,
                              const Expr *InvalidationExpr) override {
    auto InvalidationDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                                ? diag::note_lifetime_safety_freed_here
                                : diag::note_lifetime_safety_invalidated_here;
    S.Diag(IssueExpr->getExprLoc(),
           diag::warn_lifetime_safety_invalidated_field)
        << false << IssueExpr->getSourceRange();
    S.Diag(InvalidationExpr->getExprLoc(), InvalidationDiag)
        << InvalidationExpr->getSourceRange();
    S.Diag(DanglingField->getLocation(),
           diag::note_lifetime_safety_dangling_field_here)
        << DanglingField->getEndLoc();
  }

  void reportInvalidatedField(const ParmVarDecl *PVD,
                              const FieldDecl *DanglingField,
                              const Expr *InvalidationExpr) override {
    auto InvalidationDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                                ? diag::note_lifetime_safety_freed_here
                                : diag::note_lifetime_safety_invalidated_here;
    S.Diag(PVD->getSourceRange().getBegin(),
           diag::warn_lifetime_safety_invalidated_field)
        << true << PVD->getSourceRange();
    S.Diag(InvalidationExpr->getExprLoc(), InvalidationDiag)
        << InvalidationExpr->getSourceRange();
    S.Diag(DanglingField->getLocation(),
           diag::note_lifetime_safety_dangling_field_here)
        << DanglingField->getEndLoc();
  }

  void reportInvalidatedGlobal(const Expr *IssueExpr,
                               const VarDecl *DanglingGlobal,
                               const Expr *InvalidationExpr) override {
    auto InvalidationDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                                ? diag::note_lifetime_safety_freed_here
                                : diag::note_lifetime_safety_invalidated_here;
    S.Diag(IssueExpr->getExprLoc(),
           diag::warn_lifetime_safety_invalidated_global)
        << false << IssueExpr->getSourceRange();
    S.Diag(InvalidationExpr->getExprLoc(), InvalidationDiag)
        << InvalidationExpr->getSourceRange();
    if (DanglingGlobal->isStaticLocal() || DanglingGlobal->isStaticDataMember())
      S.Diag(DanglingGlobal->getLocation(),
             diag::note_lifetime_safety_dangling_static_here)
          << DanglingGlobal->getEndLoc();
    else
      S.Diag(DanglingGlobal->getLocation(),
             diag::note_lifetime_safety_dangling_global_here)
          << DanglingGlobal->getEndLoc();
  }

  void reportInvalidatedGlobal(const ParmVarDecl *PVD,
                               const VarDecl *DanglingGlobal,
                               const Expr *InvalidationExpr) override {
    auto InvalidationDiag = isa<CXXDeleteExpr>(InvalidationExpr)
                                ? diag::note_lifetime_safety_freed_here
                                : diag::note_lifetime_safety_invalidated_here;
    S.Diag(PVD->getSourceRange().getBegin(),
           diag::warn_lifetime_safety_invalidated_global)
        << true << PVD->getSourceRange();
    S.Diag(InvalidationExpr->getExprLoc(), InvalidationDiag)
        << InvalidationExpr->getSourceRange();
    if (DanglingGlobal->isStaticLocal() || DanglingGlobal->isStaticDataMember())
      S.Diag(DanglingGlobal->getLocation(),
             diag::note_lifetime_safety_dangling_static_here)
          << DanglingGlobal->getEndLoc();
    else
      S.Diag(DanglingGlobal->getLocation(),
             diag::note_lifetime_safety_dangling_global_here)
          << DanglingGlobal->getEndLoc();
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
    S.Diag(Attr->getLocation(),
           diag::warn_lifetime_safety_lifetimebound_violation)
        << HasName << ParamName << Attr->getRange();
  }

  void reportLifetimeboundViolation(
      const CXXMethodDecl *MDWithLifetimebound) override {
    const auto *Attr =
        getImplicitObjectParamLifetimeBoundAttr(MDWithLifetimebound);
    assert(Attr && "Expected lifetimebound attribute");
    S.Diag(Attr->getLocation(),
           diag::warn_lifetime_safety_lifetimebound_violation)
        << 2 << "" << Attr->getRange();
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

  void reportImmortalViolation(const FunctionDecl *FD,
                               unsigned Subject) override {
    const auto *Attr = FD->getAttr<LifetimeImmortalAttr>();
    SourceLocation Loc = Attr ? Attr->getLocation() : FD->getLocation();
    S.Diag(Loc, diag::warn_lifetime_safety_immortal_violation) << Subject;
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

  void reportUnannotatedThisReturn(const CXXMethodDecl *MD) override {
    S.Diag(MD->getLocation(),
           diag::warn_lifetime_safety_unannotated_this_return)
        << MD->getReturnType() << MD->getSourceRange();
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

  void reportGlobalCapture(const ParmVarDecl *PVD, bool IsUnknown) override {
    S.Diag(PVD->getLocation(), diag::warn_lifetime_safety_global_capture)
        << IsUnknown << PVD->getSourceRange();
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
  void reportUnsupportedStoreDestination(const Expr *E) override {
    S.Diag(E->getExprLoc(), diag::warn_lifetime_safety_unsupported_store)
        << E->getSourceRange();
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
    // TODO: Handle other expression types.
    return "";
  }

  Sema &S;
};

} // namespace clang::lifetimes

#endif // LLVM_CLANG_LIB_SEMA_SEMALIFETIMESAFETY_H
