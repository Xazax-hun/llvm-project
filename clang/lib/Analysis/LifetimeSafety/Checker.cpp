
//===- Checker.cpp - C++ Lifetime Safety Checker ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the LifetimeChecker, which detects use-after-free
// errors by checking if live origins hold loans that have expired.
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/Analyses/LifetimeSafety/Checker.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LiveOrigins.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LoanPropagation.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Loans.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TimeProfiler.h"

namespace clang::lifetimes::internal {

static bool causingFactDominatesExpiry(LivenessKind K) {
  switch (K) {
  case LivenessKind::Must:
    return true;
  case LivenessKind::Maybe:
  case LivenessKind::Dead:
    return false;
  }
  llvm_unreachable("unknown liveness kind");
}

namespace {

/// Returns true if peeling all pointer (and array) levels of \p T yields a
/// character type -- i.e. T is `char**`, `char*[]`, etc. Used to exempt main's
/// `argv`/`envp` parameters, whose multi-level type is mandated by the
/// language.
static bool isCharacterPointerChain(QualType T, const ASTContext &Ctx) {
  T = Ctx.getBaseElementType(T);
  if (!T->isPointerType())
    return false;
  while (T->isPointerType())
    T = T->getPointeeType();
  return T->isAnyCharacterType();
}

/// Collects the compiler-introduced range variables ('auto&& __range = ...') of
/// every range-based for statement reachable from \p S.
static void collectRangeForRangeVars(const Stmt *S,
                                     llvm::DenseSet<const VarDecl *> &Out) {
  if (!S)
    return;
  if (const auto *FRS = dyn_cast<CXXForRangeStmt>(S))
    if (const DeclStmt *DS = FRS->getRangeStmt())
      if (const auto *VD = dyn_cast_or_null<VarDecl>(DS->getSingleDecl()))
        Out.insert(VD);
  for (const Stmt *Child : S->children())
    collectRangeForRangeVars(Child, Out);
}

/// Struct to store the complete context for a potential lifetime violation.
struct PendingWarning {
  SourceLocation ExpiryLoc; // Where the loan expired.
  llvm::PointerUnion<const UseFact *, const OriginEscapesFact *> CausingFact;
  const Expr *MovedExpr;
  const Expr *InvalidatedByExpr;
  bool CausingFactDominatesExpiry;
};

/// If `AP` names an object -- the implicit `this`, or a variable/parameter of
/// record type -- returns that record; otherwise null. The safe model treats
/// invalidating an object (e.g. a non-const member call) as also invalidating
/// borrows into its (possibly transitive / inherited) owner fields.
static const CXXRecordDecl *invalidatedObjectRecord(const AccessPath &AP) {
  if (const CXXMethodDecl *MD = AP.getAsPlaceholderThis())
    return MD->getParent();
  if (const ValueDecl *VD = AP.getAsValueDecl())
    if (!isa<FieldDecl>(VD))
      return VD->getType().getNonReferenceType()->getAsCXXRecordDecl();
  return nullptr;
}

/// True if `Field` is reachable as a (possibly transitive / inherited) data
/// member of `RD`. `Visited` cuts cycles.
static bool recordReachesField(const CXXRecordDecl *RD, const FieldDecl *Field,
                               llvm::SmallPtrSet<const CXXRecordDecl *, 8> &Visited) {
  if (!RD || !RD->hasDefinition())
    return false;
  if (!Visited.insert(RD->getCanonicalDecl()).second)
    return false;
  if (Field->getParent()->getCanonicalDecl() == RD->getCanonicalDecl())
    return true;
  for (const CXXBaseSpecifier &B : RD->bases())
    if (recordReachesField(B.getType()->getAsCXXRecordDecl(), Field, Visited))
      return true;
  for (const FieldDecl *FD : RD->fields()) {
    QualType FT = FD->getType().getNonReferenceType();
    while (FT->isArrayType())
      FT = FT->getAsArrayTypeUnsafe()->getElementType();
    if (recordReachesField(FT->getAsCXXRecordDecl(), Field, Visited))
      return true;
  }
  return false;
}

/// True if loan `L` is a field-rooted borrow whose field is a (possibly
/// transitive / inherited) member of `RD`. Field loans are FieldDecl-rooted and
/// instance-insensitive, so this can match a field of a *different* instance of
/// the same type -- a deliberate over-approximation under the safe model.
static bool isFieldBorrowOf(const Loan *L, const CXXRecordDecl *RD) {
  if (!RD)
    return false;
  if (const ValueDecl *VD = L->getAccessPath().getAsValueDecl())
    if (const auto *FD = dyn_cast<FieldDecl>(VD)) {
      llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
      return recordReachesField(RD, FD, Visited);
    }
  return false;
}

using AnnotationTarget =
    llvm::PointerUnion<const ParmVarDecl *, const CXXMethodDecl *>;
using EscapingTarget = LifetimeSafetySemaHelper::EscapingTarget;

class LifetimeChecker {
private:
  llvm::DenseMap<LoanID, PendingWarning> FinalWarningsMap;
  llvm::DenseMap<AnnotationTarget, EscapingTarget> AnnotationWarningsMap;
  llvm::DenseMap<const ParmVarDecl *, EscapingTarget> NoescapeWarningsMap;
  /// Borrows of the implicit object (`this`) or one of its fields that escape
  /// to global/static storage from a method. The global is caller-independent
  /// and outlives the call, but the borrowed object's lifetime is the caller's,
  /// so such an escape may dangle. Keyed by the global to de-duplicate; the
  /// value records whether the borrow was of `this` (false) or a field (true)
  /// and a source location to anchor the diagnostic.
  struct ThisEscapeToGlobal {
    bool IsField;
    SourceLocation Loc;
  };
  llvm::DenseMap<const VarDecl *, ThisEscapeToGlobal> ThisEscapesToGlobalMap;
  /// Parameters annotated [[clang::lifetimebound]] or [[clang::lifetime_capture_by]]
  /// (to something other than `global`) whose borrow nonetheless escapes to
  /// global/static storage. Those annotations describe a return/capture
  /// relationship, not a global capture, so the caller is unaware the global
  /// now aliases the argument. Keyed by parameter to de-duplicate.
  llvm::DenseMap<const ParmVarDecl *, const VarDecl *>
      AnnotatedParamEscapesToGlobalMap;
  llvm::DenseSet<const Decl *> VerifiedLiftimeboundEscapes;
  /// For a [[clang::lifetime_immortal]] function: the worst offending subject
  /// its return value borrows (0 = local/temporary, 1 = parameter, 2 = this);
  /// -1 when none seen. A non-immortal return makes the immortal promise a lie.
  int ImmortalViolationSubject = -1;
  /// For a [[clang::lifetime_immortal]] function: whether the return value
  /// carries an untracked (Unknown) loan the analysis cannot prove is immortal
  /// (e.g. a local borrow laundered through a borrow-returning call such as
  /// string_view::substr). Reported (subject 3) only when no concrete offending
  /// subject above was found, so a precise local/param/this report is preferred.
  bool ImmortalReturnsUntracked = false;
  /// Source locations already reported as lost-loan, to avoid duplicate
  /// soundness warnings when several uses (e.g. a DeclRefExpr and its
  /// lvalue-to-rvalue cast) share a location.
  llvm::DenseSet<SourceLocation> ReportedLostLoanLocs;
  /// Source locations already reported as borrowing from a mutable global.
  llvm::DenseSet<SourceLocation> ReportedMutableGlobalLocs;
  /// Expressions already reported as an untracked construct, to avoid duplicate
  /// soundness warnings when a construct is visited more than once.
  llvm::DenseSet<const Expr *> ReportedUntrackedExprs;
  /// Declarations already reported as an untracked construct.
  llvm::DenseSet<const ValueDecl *> ReportedUntrackedDecls;
  /// Source locations already reported as an untracked construct, for facts
  /// anchored to a statement rather than an expression or declaration.
  llvm::DenseSet<SourceLocation> ReportedUntrackedLocs;
  /// (loan, operation) pairs already reported as assumed-invalidation, to avoid
  /// duplicate warnings when several live origins hold the same borrow.
  llvm::DenseSet<std::pair<unsigned, const Expr *>> ReportedAssumedInval;
  /// (aliased storage, call) pairs already reported as an argument overlap, to
  /// avoid a duplicate when two aliasing reference arguments are symmetric.
  llvm::DenseSet<std::pair<const void *, const Expr *>> ReportedArgOverlap;
  /// Field-store expressions already reported as self-referential.
  llvm::DenseSet<const Expr *> ReportedSelfRefStores;
  /// Assumed-invalidation candidates collected during the fact walk, emitted
  /// after the precise warnings are finalized.
  llvm::SmallVector<std::pair<LoanID, const Expr *>> PendingAssumedInval;
  const LoanPropagationAnalysis &LoanPropagation;
  const MovedLoansAnalysis &MovedLoans;
  const LiveOriginsAnalysis &LiveOrigins;
  FactManager &FactMgr;
  LifetimeSafetySemaHelper *SemaHelper;
  ASTContext &AST;
  const Decl *FD;

  static SourceLocation
  GetFactLoc(llvm::PointerUnion<const UseFact *, const OriginEscapesFact *> F) {
    if (const auto *UF = F.dyn_cast<const UseFact *>())
      return UF->isImplicit() ? UF->getImplicitLoc()
                              : UF->getUseExpr()->getExprLoc();
    if (const auto *OEF = F.dyn_cast<const OriginEscapesFact *>()) {
      if (auto *ReturnEsc = dyn_cast<ReturnEscapeFact>(OEF))
        return ReturnEsc->getReturnExpr()->getExprLoc();
      if (auto *FieldEsc = dyn_cast<FieldEscapeFact>(OEF))
        return FieldEsc->getFieldDecl()->getLocation();
    }
    llvm_unreachable("unhandled causing fact in PointerUnion");
  }

public:
  LifetimeChecker(const LoanPropagationAnalysis &LoanPropagation,
                  const MovedLoansAnalysis &MovedLoans,
                  const LiveOriginsAnalysis &LiveOrigins, FactManager &FM,
                  AnalysisDeclContext &ADC,
                  LifetimeSafetySemaHelper *SemaHelper)
      : LoanPropagation(LoanPropagation), MovedLoans(MovedLoans),
        LiveOrigins(LiveOrigins), FactMgr(FM), SemaHelper(SemaHelper),
        AST(ADC.getASTContext()), FD(ADC.getDecl()) {
    for (const CFGBlock *B : *ADC.getAnalysis<PostOrderCFGView>())
      for (const Fact *F : FactMgr.getFacts(B))
        if (const auto *EF = F->getAs<ExpireFact>())
          checkExpiry(EF);
        else if (const auto *IOF = F->getAs<InvalidateOriginFact>()) {
          if (IOF->isAssumed())
            checkAssumedInvalidation(IOF);
          else {
            checkInvalidation(IOF);
            if (IOF->isDeallocation())
              checkNakedDeallocation(IOF);
          }
        }
        else if (const auto *OEF = F->getAs<OriginEscapesFact>()) {
          checkAnnotations(OEF);
          checkBorrowFromMutableGlobal(OEF);
        }
        else if (const auto *UF = F->getAs<UseFact>()) {
          checkLostLoan(UF);
          checkBorrowFromMutableGlobal(UF);
        }
        else if (const auto *UCF = F->getAs<UntrackedConstructFact>())
          recordUntrackedConstruct(UCF);
        else if (const auto *FSF = F->getAs<FieldStoreFact>())
          checkSelfReferentialStore(FSF);
        else if (const auto *AOF = F->getAs<ArgOverlapFact>())
          checkArgumentOverlap(AOF);
    issuePendingWarnings();
    issueAssumedInvalidations();
    checkUnannotatedParams();
    checkGlobalCaptureAnnotations();
    checkMultiLevelIndirection();
    suggestAnnotations();
    reportNoescapeViolations();
    reportThisEscapesToGlobal();
    reportLifetimeboundViolations();
    reportMisplacedLifetimebound();
    //  Annotation inference is currently guarded by a frontend flag. In the
    //  future, this might be replaced by a design that differentiates between
    //  explicit and inferred findings with separate warning groups.
    if (AST.getLangOpts().EnableLifetimeSafetyInference)
      inferAnnotations();
  }

  /// Returns true if \p PVD is annotated [[clang::lifetime_capture_by(global)]],
  /// which documents that the parameter may be captured by global storage.
  static bool capturesGlobal(const ParmVarDecl *PVD) {
    if (const auto *A = PVD->getAttr<LifetimeCaptureByAttr>())
      for (int Idx : A->params())
        if (Idx == LifetimeCaptureByAttr::Global)
          return true;
    return false;
  }

  /// Checks if an escaping origin holds a placeholder loan, indicating a
  /// missing [[clang::lifetimebound]] annotation or a violation of
  /// [[clang::noescape]].
  void checkAnnotations(const OriginEscapesFact *OEF) {
    OriginID EscapedOID = OEF->getEscapedOriginID();
    LoanSet EscapedLoans = LoanPropagation.getLoans(EscapedOID, OEF);
    // A [[clang::lifetime_immortal]] function promises its return value lives
    // forever. Verify the body: a returned borrow must be of genuinely immortal
    // storage (immortal/heap, or a global/static variable). A borrow of a
    // parameter, the implicit object, or a local/temporary is a lie -- callers
    // trust the promise and may keep the result past that object's lifetime.
    if (isa<ReturnEscapeFact>(OEF) && FD->hasAttr<LifetimeImmortalAttr>()) {
      for (LoanID LID : EscapedLoans) {
        const AccessPath &AP = FactMgr.getLoanMgr().getLoan(LID)->getAccessPath();
        switch (AP.getKind()) {
        case AccessPath::Kind::Immortal:
        case AccessPath::Kind::NewAllocation:
          continue;
        case AccessPath::Kind::Unknown:
          // Untracked: the analysis cannot prove this borrow is immortal. It may
          // be a local laundered through a borrow-returning call (e.g.
          // string_view::substr yields an Unknown loan even when it views a
          // local). An immortal function must return only provably-immortal
          // storage, so an unverifiable borrow is a violation too.
          ImmortalReturnsUntracked = true;
          continue;
        case AccessPath::Kind::PlaceholderThis:
          ImmortalViolationSubject = std::max(ImmortalViolationSubject, 2);
          continue;
        case AccessPath::Kind::PlaceholderParam:
          ImmortalViolationSubject = std::max(ImmortalViolationSubject, 1);
          continue;
        case AccessPath::Kind::ValueDecl: {
          const auto *VD = dyn_cast_or_null<VarDecl>(AP.getAsValueDecl());
          // A global/static variable is immortal; a local/field is not.
          if (VD && VD->hasGlobalStorage())
            continue;
          ImmortalViolationSubject = std::max(ImmortalViolationSubject, 0);
          continue;
        }
        case AccessPath::Kind::MaterializeTemporary:
        case AccessPath::Kind::Uninitialized:
          ImmortalViolationSubject = std::max(ImmortalViolationSubject, 0);
          continue;
        }
      }
    }
    auto CheckParam = [&](const ParmVarDecl *PVD, bool IsMoved) {
      // NoEscape param should not escape.
      if (PVD->hasAttr<NoEscapeAttr>()) {
        if (auto *ReturnEsc = dyn_cast<ReturnEscapeFact>(OEF))
          NoescapeWarningsMap.try_emplace(PVD, ReturnEsc->getReturnExpr());
        if (auto *FieldEsc = dyn_cast<FieldEscapeFact>(OEF))
          NoescapeWarningsMap.try_emplace(PVD, FieldEsc->getFieldDecl());
        if (auto *GlobalEsc = dyn_cast<GlobalEscapeFact>(OEF))
          NoescapeWarningsMap.try_emplace(PVD, GlobalEsc->getGlobal());
        return;
      }
      // Skip annotation suggestion for moved loans, as ownership transfer
      // obscures the lifetime relationship (e.g., shared_ptr from unique_ptr).
      if (IsMoved)
        return;
      if (PVD->hasAttr<LifetimeBoundAttr>()) {
        // Track that this lifetimebound parameter correctly escapes.
        if (isa<ReturnEscapeFact>(OEF))
          VerifiedLiftimeboundEscapes.insert(PVD);
      } else {
        // Otherwise, suggest lifetimebound for parameter escaping through
        // return or a field in constructor.
        if (auto *ReturnEsc = dyn_cast<ReturnEscapeFact>(OEF))
          AnnotationWarningsMap.try_emplace(PVD, ReturnEsc->getReturnExpr());
        else if (auto *FieldEsc = dyn_cast<FieldEscapeFact>(OEF);
                 FieldEsc && isa<CXXConstructorDecl>(FD))
          AnnotationWarningsMap.try_emplace(PVD, FieldEsc->getFieldDecl());
      }
      // TODO: Suggest lifetime_capture_by(this) for parameter escaping to a
      // field!
    };
    auto CheckImplicitThis = [&](const CXXMethodDecl *MD) {
      if (auto *ReturnEsc = dyn_cast<ReturnEscapeFact>(OEF)) {
        if (implicitObjectParamIsLifetimeBound(MD))
          VerifiedLiftimeboundEscapes.insert(MD);
        else
          AnnotationWarningsMap.try_emplace(MD, ReturnEsc->getReturnExpr());
      }
    };
    auto MovedAtEscape = MovedLoans.getMovedLoans(OEF);
    for (LoanID LID : EscapedLoans) {
      const Loan *L = FactMgr.getLoanMgr().getLoan(LID);
      const AccessPath &AP = L->getAccessPath();
      // Safe-model rule: a borrow of the implicit object (`this`) or one of its
      // fields must not escape to global/static storage. Intra-procedurally the
      // object is caller-scope (a placeholder that never expires), so such a
      // store is otherwise silently accepted even though the global outlives the
      // caller's object. Flag it here.
      if (const auto *GlobalEsc = dyn_cast<GlobalEscapeFact>(OEF)) {
        bool IsThis = AP.getAsPlaceholderThis() != nullptr;
        const auto *VD = AP.getAsValueDecl();
        bool IsField = VD && isa<FieldDecl>(VD);
        if (IsThis || IsField) {
          SourceLocation Loc = L->getIssuingExpr()
                                   ? L->getIssuingExpr()->getExprLoc()
                                   : FD->getLocation();
          ThisEscapesToGlobalMap.try_emplace(GlobalEsc->getGlobal(),
                                             ThisEscapeToGlobal{IsField, Loc});
        }
        // A parameter annotated [[clang::lifetimebound]] or
        // [[clang::lifetime_capture_by]] (to something other than `global`)
        // describes a return/capture relationship, not a global capture; its
        // escape to a global is therefore uncovered by the annotation. An
        // unannotated parameter is already reported by checkUnannotatedParams,
        // and a [[clang::noescape]] one by reportNoescapeViolations, so they are
        // excluded here.
        if (const auto *PVD = AP.getAsPlaceholderParam();
            PVD && !PVD->hasAttr<NoEscapeAttr>() &&
            (PVD->hasAttr<LifetimeBoundAttr>() ||
             PVD->hasAttr<LifetimeCaptureByAttr>()) &&
            !capturesGlobal(PVD))
          AnnotatedParamEscapesToGlobalMap.try_emplace(PVD,
                                                       GlobalEsc->getGlobal());
      }
      if (const auto *PVD = AP.getAsPlaceholderParam())
        CheckParam(PVD, /*IsMoved=*/MovedAtEscape.lookup(LID));
      else if (const auto *MD = AP.getAsPlaceholderThis())
        CheckImplicitThis(MD);
    }
  }

  /// Checks for use-after-free & use-after-return errors when an access path
  /// expires (e.g., a variable goes out of scope).
  ///
  /// When a path expires, all loans having this path expires.
  /// This method examines all live origins and reports warnings for loans they
  /// hold that are prefixed by the expired path.
  void checkExpiry(const ExpireFact *EF) {
    const AccessPath &ExpiredPath = EF->getAccessPath();
    LivenessMap Origins = LiveOrigins.getLiveOriginsAt(EF);
    for (auto &[OID, LiveInfo] : Origins) {
      LoanSet HeldLoans = LoanPropagation.getLoans(OID, EF);
      for (LoanID HeldLoanID : HeldLoans) {
        const Loan *HeldLoan = FactMgr.getLoanMgr().getLoan(HeldLoanID);
        if (ExpiredPath != HeldLoan->getAccessPath())
          continue;
        // HeldLoan is expired because its AccessPath is expired.
        PendingWarning &CurWarning = FinalWarningsMap[HeldLoan->getID()];
        const Expr *MovedExpr = nullptr;
        if (auto *ME = MovedLoans.getMovedLoans(EF).lookup(HeldLoanID))
          MovedExpr = *ME;
        // Skip if we already have a dominating causing fact.
        if (CurWarning.CausingFactDominatesExpiry)
          continue;
        if (causingFactDominatesExpiry(LiveInfo.Kind))
          CurWarning.CausingFactDominatesExpiry = true;
        CurWarning.CausingFact = LiveInfo.CausingFact;
        CurWarning.ExpiryLoc = EF->getExpiryLoc();
        CurWarning.MovedExpr = MovedExpr;
        CurWarning.InvalidatedByExpr = nullptr;
      }
    }
  }

  /// Checks for use-after-invalidation errors when a container is modified.
  ///
  /// This method identifies origins that are live at the point of invalidation
  /// and checks if they hold loans that are invalidated by the operation
  /// (e.g., iterators into a vector that is being pushed to).
  void checkInvalidation(const InvalidateOriginFact *IOF) {
    OriginID InvalidatedOrigin = IOF->getInvalidatedOrigin();
    /// Get loans directly pointing to the invalidated container
    LoanSet DirectlyInvalidatedLoans =
        LoanPropagation.getLoans(InvalidatedOrigin, IOF);
    const FieldDecl *MutatedField = IOF->getMutatedField();
    auto LoanAP = [&](LoanID L) -> const AccessPath & {
      return FactMgr.getLoanMgr().getLoan(L)->getAccessPath();
    };
    // Exact match: the loan directly borrows the invalidated storage. For a
    // field mutation (`s.buf.append(...)`) this is the specific field; for a
    // container mutation it is the receiver's loans.
    auto IsExactInvalidated = [&](LoanID L) {
      if (MutatedField)
        return LoanAP(L).getAsValueDecl() == MutatedField;
      for (LoanID InvalidID : DirectlyInvalidatedLoans)
        if (LoanAP(InvalidID) == LoanAP(L))
          return true;
      return false;
    };
    // True if origin `OID` is a borrow *into* an object of record `ObjRD` (a
    // view, or a raw pointer/reference whose pointee is some sub-buffer), as
    // opposed to a pointer *at* the whole object (pointee is `ObjRD` itself,
    // which a field mutation does not dangle).
    auto OriginBorrowsInto = [&](OriginID OID, const CXXRecordDecl *ObjRD) {
      const Type *Ty = FactMgr.getOriginMgr().getOrigin(OID).Ty;
      if (!Ty)
        return false;
      QualType QT(Ty, 0);
      if (isGslPointerType(QT))
        return true; // a view (string_view/span/iterator)
      if (!QT->isPointerType() && !QT->isReferenceType())
        return false;
      const CXXRecordDecl *PointeeRD =
          QT->getPointeeType()->getAsCXXRecordDecl();
      return !PointeeRD ||
             PointeeRD->getCanonicalDecl() != ObjRD->getCanonicalDecl();
    };
    // For each live origin, check if it holds an invalidated loan and report.
    LivenessMap Origins = LiveOrigins.getLiveOriginsAt(IOF);
    for (auto &[OID, LiveInfo] : Origins) {
      LoanSet HeldLoans = LoanPropagation.getLoans(OID, IOF);
      llvm::SmallVector<LoanID, 2> Invalidated;
      for (LoanID L : HeldLoans)
        if (IsExactInvalidated(L))
          Invalidated.push_back(L);

      // Conservative case for a field mutation: an *imprecise* borrow into the
      // object is also invalidated. Such a borrow (a view, or a raw
      // pointer/reference into the object) holds the enclosing object's loan
      // but no precise field loan -- e.g. one produced by a lifetimebound
      // accessor (`v = doc.getView()`, `p = doc.data()`), where we do not know
      // which subobject it borrows, so any owner-field mutation of the object
      // may invalidate it. A borrow that directly named a field carries that
      // field's loan and is matched exactly above (so a sibling-field mutation
      // does not reach it); a pointer *at* the whole object is excluded.
      if (MutatedField && Invalidated.empty()) {
        bool HoldsFieldLoan = false;
        LoanID ObjectLoan;
        const CXXRecordDecl *ObjRD = nullptr;
        for (LoanID L : HeldLoans) {
          const AccessPath &AP = LoanAP(L);
          if (const ValueDecl *VD = AP.getAsValueDecl(); VD && isa<FieldDecl>(VD))
            HoldsFieldLoan = true;
          if (!ObjRD)
            if (const CXXRecordDecl *RD = invalidatedObjectRecord(AP)) {
              llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
              if (recordReachesField(RD, MutatedField, Visited)) {
                ObjectLoan = L;
                ObjRD = RD;
              }
            }
        }
        if (ObjRD && !HoldsFieldLoan && OriginBorrowsInto(OID, ObjRD))
          Invalidated.push_back(ObjectLoan);
      }

      for (LoanID LiveLoanID : Invalidated) {
        bool CurDomination = causingFactDominatesExpiry(LiveInfo.Kind);
        bool LastDomination =
            FinalWarningsMap.lookup(LiveLoanID).CausingFactDominatesExpiry;
        if (!LastDomination) {
          FinalWarningsMap[LiveLoanID] = {
              /*ExpiryLoc=*/{},
              /*CausingFact=*/LiveInfo.CausingFact,
              /*MovedExpr=*/nullptr,
              /*InvalidatedByExpr=*/IOF->getInvalidationExpr(),
              /*CausingFactDominatesExpiry=*/CurDomination};
        }
      }
    }
  }

  /// Soundness check: a `delete`/`free`/`std::destroy_at` is "naked" if any
  /// loan flowing into the deallocated pointer is not a heap allocation -- the
  /// analysis cannot then prove the deallocation refers to a live, unaliased
  /// heap allocation. This is intentionally strict: every loan must be a heap
  /// allocation. Deallocations inside a destructor are exempt (freeing owned
  /// members there is the normal ownership pattern).
  void checkNakedDeallocation(const InvalidateOriginFact *IOF) {
    if (!SemaHelper || isa_and_present<CXXDestructorDecl>(FD))
      return;
    // Strict: a deallocation is safe only if every borrow flowing into it is a
    // tracked heap allocation. A non-heap loan (e.g. a stack borrow) cannot be
    // freed; an *empty* loan set means the allocation was never seen -- e.g.
    // deleting through a member or parameter pointer -- which is equally
    // unverifiable (the pointer may be aliased or not heap-owned).
    LoanSet Loans = LoanPropagation.getLoans(IOF->getInvalidatedOrigin(), IOF);
    bool Safe = !Loans.isEmpty();
    for (LoanID LID : Loans)
      if (!FactMgr.getLoanMgr()
               .getLoan(LID)
               ->getAccessPath()
               .isHeapAllocation()) {
        Safe = false;
        break;
      }
    if (!Safe)
      SemaHelper->reportNakedDeallocation(IOF->getInvalidationExpr());
  }

  /// Soundness check for "assumed" invalidations (a non-const operation on an
  /// owner, or passing an owner to a non-const pointer/reference parameter).
  /// Mirrors checkInvalidation's borrow-matching but collects candidates to be
  /// emitted after the precise warnings are finalized, so that a borrow already
  /// reported as a known invalidation is not also flagged here.
  void checkAssumedInvalidation(const InvalidateOriginFact *IOF) {
    if (!SemaHelper)
      return;
    LoanSet DirectlyInvalidatedLoans =
        LoanPropagation.getLoans(IOF->getInvalidatedOrigin(), IOF);
    if (DirectlyInvalidatedLoans.isEmpty())
      return;
    auto IsInvalidated = [&](const Loan *L) {
      for (LoanID InvalidID : DirectlyInvalidatedLoans) {
        const AccessPath &AP =
            FactMgr.getLoanMgr().getLoan(InvalidID)->getAccessPath();
        if (AP == L->getAccessPath())
          return true;
        // Invalidating an object also invalidates borrows into its owner fields
        // (a non-const member call may reallocate one). Same-instance borrows
        // also carry the object loan and match above; this additionally covers
        // transitive/inherited fields and the cross-instance case.
        if (isFieldBorrowOf(L, invalidatedObjectRecord(AP)))
          return true;
      }
      return false;
    };
    for (auto &[OID, LiveInfo] : LiveOrigins.getLiveOriginsAt(IOF)) {
      for (LoanID LiveLoanID : LoanPropagation.getLoans(OID, IOF)) {
        const Loan *L = FactMgr.getLoanMgr().getLoan(LiveLoanID);
        if (!IsInvalidated(L))
          continue;
        // Only a loan with a reportable anchor (an issuing expression or a
        // placeholder parameter) can produce a diagnostic; skip e.g. the `$this`
        // placeholder loan, which carries neither.
        if (!L->getIssuingExpr() &&
            !L->getAccessPath().getAsPlaceholderParam())
          continue;
        // Report each invalidated view (origin) at most once per operation: a
        // multi-level field borrow (`o.inner.s`) carries both the field loan
        // and the enclosing-object loan, but it is a single captured value.
        if (ReportedAssumedInval.insert({LiveLoanID.Value, IOF->getInvalidationExpr()})
                .second)
          PendingAssumedInval.push_back({LiveLoanID, IOF->getInvalidationExpr()});
        break;
      }
    }
  }

  /// Emits the collected assumed-invalidation warnings, skipping any borrow
  /// that was already reported as a known (precise) invalidation.
  void issueAssumedInvalidations() {
    if (!SemaHelper)
      return;
    for (auto &[LID, OperationExpr] : PendingAssumedInval) {
      // If this borrow is already reported as a known invalidation, the
      // lower-confidence assumed warning would be redundant.
      auto It = FinalWarningsMap.find(LID);
      if (It != FinalWarningsMap.end() && It->second.InvalidatedByExpr)
        continue;
      const Loan *L = FactMgr.getLoanMgr().getLoan(LID);
      if (const Expr *IssueExpr = L->getIssuingExpr())
        SemaHelper->reportAssumedInvalidation(IssueExpr, OperationExpr);
      else if (const ParmVarDecl *PVD =
                   L->getAccessPath().getAsPlaceholderParam())
        SemaHelper->reportAssumedInvalidation(PVD, OperationExpr);
    }
  }

  /// indicates a borrow was lost because some construct was not modeled during
  /// loan propagation (or the pointer is null/uninitialized, which is equally
  /// untracked). Reports it so that, with the warning enabled as an error, the
  /// analysis never silently fails to account for a borrow.
  void checkLostLoan(const UseFact *UF) {
    if (!SemaHelper || UF->isWritten() || UF->isImplicit())
      return;
    const Expr *UseExpr = UF->getUseExpr();
    // Skip implicit nodes (e.g. an lvalue-to-rvalue cast) that duplicate an
    // explicit use at the same location and carry no useful diagnostic subject.
    // IgnoreImplicit() returns a different node exactly when the use expression
    // is itself an implicit node.
    if (UseExpr->IgnoreImplicit() != UseExpr)
      return;
    const OriginNode *OL = UF->getUsedOrigins();
    if (!OL)
      return;
    // An unknown-ownership user type (e.g. `struct Holder { int *p; };`) is
    // opaque: it is already reported via UnknownOwnership, and its value
    // legitimately carries no tracked loan. Reporting a "lost loan" on it would
    // be a spurious second diagnostic for the same modeling gap.
    if (const Type *Ty = FactMgr.getOriginMgr().getOrigin(OL->getOriginID()).Ty;
        Ty && isUnknownOwnershipType(QualType(Ty, 0),
                                     FactMgr.getUnknownOwnershipCache()))
      return;
    // A borrow is lost if the origin holds no loan at all, OR if it holds an
    // Unknown loan -- an untracked borrow (e.g. a non-lifetimebound view
    // returned by std::string_view::substr). The Unknown loan survives dataflow
    // joins, so it is detected even when a valid borrow on another path would
    // otherwise mask the loss in the (union) loan set.
    LoanSet Loans = LoanPropagation.getLoans(OL->getOriginID(), UF);
    bool Lost = Loans.isEmpty();
    for (LoanID L : Loans)
      if (FactMgr.getLoanMgr().getLoan(L)->getAccessPath().isUnknown()) {
        Lost = true;
        break;
      }
    if (!Lost)
      return;
    if (!ReportedLostLoanLocs.insert(UseExpr->getExprLoc()).second)
      return;
    SemaHelper->reportLostLoan(UseExpr);
  }

  /// Soundness: a pointer/reference/view that borrows *into* a mutable global or
  /// static owner (e.g. `int *p = &g[0];`, `p = g.data();`, `string_view sv =
  /// g_str;`, `string_view sv = g_table[i];`) can be invalidated by a mutation
  /// of that owner from anywhere -- another function or translation unit the
  /// intra-procedural analysis cannot see. The borrow surfaces as a loan rooted
  /// at the global owner regardless of how it was reached (direct, element, or
  /// accessor), so this loan-based pass covers the raw pointer/reference forms
  /// and the GSL view forms uniformly. A pointer *at* the object (`&g`) is not a
  /// borrow into its contents and stays valid, so it is excluded.
  void flagBorrowFromMutableGlobal(OriginID OID, ProgramPoint PP,
                                   const Expr *Anchor) {
    if (!SemaHelper || !Anchor)
      return;
    // A raw pointer/reference value, or a gsl::Pointer view.
    const Type *Ty = FactMgr.getOriginMgr().getOrigin(OID).Ty;
    if (!Ty)
      return;

    QualType ValueTy(Ty, 0);
    if (!ValueTy->isPointerType() && !ValueTy->isReferenceType() &&
        !isGslPointerType(ValueTy))
      return;
    for (LoanID L : LoanPropagation.getLoans(OID, PP)) {
      const AccessPath &AP = FactMgr.getLoanMgr().getLoan(L)->getAccessPath();
      const auto *VD = dyn_cast_or_null<VarDecl>(AP.getAsValueDecl());
      if (!VD || !VD->hasGlobalStorage() || VD->getType().isConstQualified() ||
          !isGslOwnerType(VD->getType()))
        continue;
      // The loan must be a borrow *into* the owner's contents, not the value of
      // the owner itself: `&g[0]`/`g.data()` borrow into `g`, whereas `&g` is a
      // pointer at the object whose storage is stable. The pointee type differs
      // from the global's own type in the former. (A gsl::Pointer view always
      // views the contents, never "is" the owner, so its non-pointer value type
      // has a null pointee and naturally falls through this guard.)
      QualType Pointee = ValueTy->getPointeeType();
      if (!Pointee.isNull() &&
          Pointee.getCanonicalType().getUnqualifiedType() ==
              VD->getType().getCanonicalType().getUnqualifiedType())
        continue; // pointer/reference AT the object, not into it
      if (!ReportedMutableGlobalLocs.insert(Anchor->getExprLoc()).second)
        return;
      SemaHelper->reportViewOnMutableGlobal(Anchor);
      return;
    }
  }

  void checkBorrowFromMutableGlobal(const UseFact *UF) {
    if (UF->isImplicit())
      return;
    // Only a genuine (non-implicit) use of the borrowing value. The implicit
    // owner->view conversion at a view's construction is itself a use whose
    // expression is implicit-wrapped; skip it so the borrow is reported at the
    // real use, not twice (once at construction, once at the use).
    if (UF->isImplicit())
      return;
    const Expr *Use = UF->getUseExpr();
    if (!Use)
      return;
    if (const OriginNode *OL = UF->getUsedOrigins())
      flagBorrowFromMutableGlobal(OL->getOriginID(), UF, Use);
  }

  /// The escape (return) form: `int *f() { return &g[0]; }`, or a view return
  /// `std::string_view f() { return g_table[i]; }`. A view return is wrapped in
  /// an implicit owner->view conversion, so strip to the underlying borrow
  /// expression for a stable, precise report location.
  void checkBorrowFromMutableGlobal(const OriginEscapesFact *OEF) {
    if (const auto *RE = dyn_cast<ReturnEscapeFact>(OEF)) {
      const Expr *Ret = RE->getReturnExpr();
      flagBorrowFromMutableGlobal(OEF->getEscapedOriginID(), OEF,
                                  Ret ? Ret->IgnoreImplicit() : nullptr);
    }
  }

  /// Translates a fact recording an unmodeled construct into the corresponding
  /// soundness diagnostic, de-duplicating per construct expression.
  void recordUntrackedConstruct(const UntrackedConstructFact *UCF) {
    if (!SemaHelper)
      return;
    const Expr *E = UCF->getConstructExpr();
    if (E && !ReportedUntrackedExprs.insert(E).second)
      return;
    const ValueDecl *D = UCF->getConstructDecl();
    if (D && !ReportedUntrackedDecls.insert(D).second)
      return;
    switch (UCF->getReason()) {
    case UntrackedConstructReason::IndirectCall:
      SemaHelper->reportIndirectCall(E);
      break;
    case UntrackedConstructReason::UnannotatedIndirection:
      SemaHelper->reportUnannotatedIndirection(E);
      break;
    case UntrackedConstructReason::MoveSilencing:
      SemaHelper->reportMoveSilencing(E);
      break;
    case UntrackedConstructReason::UnknownOwnership:
      if (D)
        SemaHelper->reportUnknownOwnership(D);
      else
        SemaHelper->reportUnknownOwnership(E);
      break;
    case UntrackedConstructReason::OwnerOfIndirection:
      if (D)
        SemaHelper->reportOwnerOfIndirection(D);
      else
        SemaHelper->reportOwnerOfIndirection(E);
      break;
    case UntrackedConstructReason::PointerOfIndirection:
      if (D)
        SemaHelper->reportPointerOfIndirection(D);
      else
        SemaHelper->reportPointerOfIndirection(E);
      break;
    case UntrackedConstructReason::Exception:
      if (ReportedUntrackedLocs.insert(UCF->getConstructLoc()).second)
        SemaHelper->reportException(UCF->getConstructLoc());
      break;
    case UntrackedConstructReason::ViewOnMutableGlobal:
      SemaHelper->reportViewOnMutableGlobal(E);
      break;
    case UntrackedConstructReason::ConstMethodIndirectMutation:
      SemaHelper->reportConstMethodIndirectMutation(E);
      break;
    }
  }

  /// Soundness check: a self-referential object. A view/pointer member is bound
  /// to a borrow of a MEMBER of the same object (e.g. `this->view = this->str;`,
  /// possibly laundered through a lifetimebound call). Mutating or moving the
  /// object invalidates the view, which the analysis cannot track once the
  /// object is passed elsewhere.
  ///
  /// The stored value must (a) borrow the very object that holds the member --
  /// it shares that object's identity loan, which pins it to the same instance
  /// -- AND (b) borrow one of that object's members (a field-rooted loan).
  /// Requiring (b) avoids flagging a lifetimebound-`this` accessor whose result
  /// borrows the object's identity but not a member (it carries only the object
  /// loan, no field loan).
  void checkSelfReferentialStore(const FieldStoreFact *FSF) {
    if (!SemaHelper)
      return;
    LoanSet Stored = LoanPropagation.getLoans(FSF->getStoredOrigin(), FSF);
    if (Stored.isEmpty())
      return;
    LoanSet Container = LoanPropagation.getLoans(FSF->getContainerOrigin(), FSF);
    for (LoanID CL : Container) {
      const AccessPath &CAP = FactMgr.getLoanMgr().getLoan(CL)->getAccessPath();
      const CXXRecordDecl *RD = invalidatedObjectRecord(CAP);
      if (!RD)
        continue;
      bool SharesObject = false, BorrowsMember = false;
      for (LoanID SL : Stored) {
        const Loan *L = FactMgr.getLoanMgr().getLoan(SL);
        if (L->getAccessPath() == CAP)
          SharesObject = true; // same instance
        if (isFieldBorrowOf(L, RD))
          BorrowsMember = true; // borrows a member of it
      }
      if (SharesObject && BorrowsMember &&
          ReportedSelfRefStores.insert(FSF->getStoreExpr()).second) {
        SemaHelper->reportSelfReferentialBorrow(FSF->getStoreExpr());
        return;
      }
    }
  }

  /// Soundness check: overlapping (aliasing) call arguments. The call may mutate
  /// the argument behind `MutatingOrigin` (an owner passed by non-const
  /// reference/pointer, or a mutating method receiver) while `BorrowOrigin` is
  /// another argument that borrows it. If their loans alias, the callee may
  /// reallocate the owner and use the now-dangling co-argument, which no
  /// annotation expresses. Reported through the assumed-invalidation pipeline.
  void checkArgumentOverlap(const ArgOverlapFact *AOF) {
    if (!SemaHelper)
      return;
    LoanSet Mutating = LoanPropagation.getLoans(AOF->getMutatingOrigin(), AOF);
    if (Mutating.isEmpty())
      return;
    for (OriginID BO : AOF->getBorrowOrigins()) {
      for (LoanID BL : LoanPropagation.getLoans(BO, AOF)) {
        const AccessPath &BAP =
            FactMgr.getLoanMgr().getLoan(BL)->getAccessPath();
        bool Aliases = false;
        for (LoanID ML : Mutating)
          if (FactMgr.getLoanMgr().getLoan(ML)->getAccessPath() == BAP) {
            Aliases = true;
            break;
          }
        if (!Aliases)
          continue;
        // The borrowing argument aliases the mutated one. Two aliasing
        // reference arguments produce symmetric facts (a->[b] and b->[a]); they
        // describe the same hazard, so de-duplicate by (aliased storage, call).
        const void *Storage = BAP.getAsValueDecl();
        if (Storage &&
            !ReportedArgOverlap.insert({Storage, AOF->getOperationExpr()})
                 .second)
          continue;
        // Reuse the assumed-invalidation reporting (the operation is the call).
        if (ReportedAssumedInval.insert({BL.Value, AOF->getOperationExpr()})
                .second)
          PendingAssumedInval.push_back({BL, AOF->getOperationExpr()});
      }
    }
  }

  /// Soundness check for the function's own signature: every parameter of
  /// origin-carrying type (raw pointer/reference, gsl::Pointer, etc.) -- and the
  /// implicit object of a member function whose result borrows it -- must carry
  /// a lifetime annotation under the "safe programming model", otherwise its
  /// lifetime contract is unspecified.
  void checkUnannotatedParams() {
    if (!SemaHelper)
      return;
    const auto *Fn = dyn_cast_or_null<FunctionDecl>(FD);
    if (!Fn)
      return;
    for (const ParmVarDecl *PVD : Fn->parameters()) {
      // Soundness: a parameter of a user-defined type whose ownership is
      // unknown (reported separately from the annotation requirement).
      if (isUnknownOwnershipType(PVD->getType(),
                                 FactMgr.getUnknownOwnershipCache())) {
        if (ReportedUntrackedDecls.insert(PVD).second)
          SemaHelper->reportUnknownOwnership(PVD);
        continue;
      }
      // Soundness: a parameter of a gsl::Owner container whose elements are
      // indirections (e.g. std::vector<int*>); per-element borrows are not
      // tracked. This holds regardless of any annotation, so check before the
      // annotation requirement and see through a reference parameter.
      if (isGslOwnerOfIndirection(PVD->getType().getNonReferenceType())) {
        if (ReportedUntrackedDecls.insert(PVD).second)
          SemaHelper->reportOwnerOfIndirection(PVD);
        continue;
      }
      // Likewise a gsl::Pointer view parameter whose pointee is an indirection
      // (e.g. std::span<int*>); the inner pointees are not tracked.
      if (isGslPointerOfIndirection(PVD->getType().getNonReferenceType())) {
        if (ReportedUntrackedDecls.insert(PVD).second)
          SemaHelper->reportPointerOfIndirection(PVD);
        continue;
      }
      if (!FactMgr.getOriginMgr().hasOrigins(PVD->getType()))
        continue;
      // Multi-level indirection is reported separately; don't double-flag.
      if (OriginNode *L = FactMgr.getOriginMgr().getOrCreateNode(PVD);
          L && L->getLength() > 1)
        continue;
      if (PVD->hasAttr<LifetimeBoundAttr>() || PVD->hasAttr<NoEscapeAttr>() ||
          PVD->hasAttr<LifetimeCaptureByAttr>())
        continue;
      SemaHelper->reportUnannotatedParam(PVD);
    }

    // The implicit object is also a parameter: an instance member function
    // (including a conversion operator) whose return type is an indirection
    // (pointer, reference, or gsl::Pointer) must declare where that borrow comes
    // from -- otherwise a (frequently implicit, e.g. `int* p = obj;`) call site
    // cannot track it. Require [[clang::lifetimebound]] on the implicit object
    // (or on a parameter the borrow may come from instead) or
    // [[clang::lifetime_immortal]] on the function. Standard-library accessors
    // are modeled via GSL recognition and are exempt.
    const auto *MD = dyn_cast<CXXMethodDecl>(Fn);
    if (!MD || !MD->isInstance() || isInStlNamespace(MD->getParent()))
      return;
    QualType RetTy = MD->getReturnType();
    if (!isPointerLikeType(RetTy) && !RetTy->isReferenceType())
      return;
    if (implicitObjectParamIsLifetimeBound(MD) ||
        MD->hasAttr<LifetimeImmortalAttr>())
      return;
    for (const ParmVarDecl *PVD : MD->parameters())
      if (PVD->hasAttr<LifetimeBoundAttr>())
        return;
    SemaHelper->reportUnannotatedThisReturn(MD);
  }

  /// Soundness check: under the "safe programming model" only a single level
  /// of indirection is supported. A declaration whose origin list has more than
  /// one level (e.g. 'int **', 'int *&') cannot be fully modeled, so flag it.
  void checkMultiLevelIndirection() {
    if (!SemaHelper)
      return;
    OriginManager &OM = FactMgr.getOriginMgr();
    // Compiler-introduced range-for range variables ('auto&& __range = ...')
    // are references that merely alias the range expression. The reference does
    // not add a real level of indirection, so it must not count toward the
    // one-level limit -- otherwise iterating a view (e.g. 'for (x : span)')
    // would be rejected as multi-level even though the view itself is a single
    // level. A genuinely untrackable range is still caught elsewhere (a
    // dangling view via use-after-scope / lost-loan on its borrow).
    llvm::DenseSet<const VarDecl *> RangeVars;
    if (const Stmt *Body = FD ? FD->getBody() : nullptr)
      collectRangeForRangeVars(Body, RangeVars);
    llvm::DenseSet<const ValueDecl *> Seen;
    llvm::SmallVector<const ValueDecl *> MultiLevel;
    auto Consider = [&](const ValueDecl *VD, OriginNode *L) {
      if (!L)
        return;
      // main's 'argv'/'envp' are 'char**' (a character pointer-chain) by
      // language mandate; exempt them from the single-indirection rule.
      // (Checked here so it applies whether the parameter is reached as a
      // parameter or via the tracked-decl map below.)
      if (const auto *P = dyn_cast<ParmVarDecl>(VD))
        if (const auto *Fn = dyn_cast<FunctionDecl>(P->getDeclContext());
            Fn && Fn->isMain() && isCharacterPointerChain(P->getType(), AST))
          return;
      unsigned Depth = L->getLength();
      if (const auto *VarD = dyn_cast<VarDecl>(VD);
          VarD && RangeVars.contains(VarD) &&
          VarD->getType()->isReferenceType() && Depth > 0)
        --Depth; // peel the alias reference of the range variable
      if (Depth > 1 && Seen.insert(VD).second)
        MultiLevel.push_back(VD);
    };
    // Parameters (including ones never used in the body).
    if (const auto *Fn = dyn_cast_or_null<FunctionDecl>(FD))
      for (const ParmVarDecl *P : Fn->parameters())
        Consider(P, OM.getOrCreateNode(P));
    // Local variables and any other tracked declarations.
    for (const auto &[VD, L] : OM.getDeclOriginLists())
      Consider(VD, L);
    // Emit in source order for deterministic diagnostics.
    llvm::sort(MultiLevel, [](const ValueDecl *A, const ValueDecl *B) {
      return A->getLocation().getRawEncoding() <
             B->getLocation().getRawEncoding();
    });
    for (const ValueDecl *VD : MultiLevel)
      SemaHelper->reportMultiLevelIndirection(VD);
  }

  void issuePendingWarnings() {
    if (!SemaHelper)
      return;
    for (const auto &[LID, Warning] : FinalWarningsMap) {
      const Loan *L = FactMgr.getLoanMgr().getLoan(LID);
      const Expr *IssueExpr = L->getIssuingExpr();
      llvm::PointerUnion<const UseFact *, const OriginEscapesFact *>
          CausingFact = Warning.CausingFact;
      const ParmVarDecl *InvalidatedPVD =
          L->getAccessPath().getAsPlaceholderParam();
      const Expr *MovedExpr = Warning.MovedExpr;
      SourceLocation ExpiryLoc = Warning.ExpiryLoc;

      if (const auto *UF = CausingFact.dyn_cast<const UseFact *>()) {
        // An implicit use (a non-trivial destructor reading a borrow at scope
        // exit) has no source expression; anchor diagnostics at its location.
        if (UF->isImplicit()) {
          if (Warning.InvalidatedByExpr) {
            if (IssueExpr)
              SemaHelper->reportUseAfterInvalidation(
                  IssueExpr, UF->getImplicitLoc(), Warning.InvalidatedByExpr);
            else if (InvalidatedPVD)
              SemaHelper->reportUseAfterInvalidation(
                  InvalidatedPVD, UF->getImplicitLoc(),
                  Warning.InvalidatedByExpr);
          } else
            SemaHelper->reportUseAfterScope(IssueExpr, UF->getImplicitLoc(),
                                            MovedExpr, ExpiryLoc);
        } else if (Warning.InvalidatedByExpr) {
          if (IssueExpr)
            // Use-after-invalidation of an object on stack.
            SemaHelper->reportUseAfterInvalidation(IssueExpr, UF->getUseExpr(),
                                                   Warning.InvalidatedByExpr);
          else if (InvalidatedPVD)
            // Use-after-invalidation of a parameter.
            SemaHelper->reportUseAfterInvalidation(
                InvalidatedPVD, UF->getUseExpr(), Warning.InvalidatedByExpr);

        } else
          // Scope-based expiry (use-after-scope).
          SemaHelper->reportUseAfterScope(IssueExpr, UF->getUseExpr(),
                                          MovedExpr, ExpiryLoc);
      } else if (const auto *OEF =
                     CausingFact.dyn_cast<const OriginEscapesFact *>()) {
        if (Warning.InvalidatedByExpr) {
          if (const auto *FieldEscape = dyn_cast<FieldEscapeFact>(OEF)) {
            // Invalidated object escapes to a field.
            if (IssueExpr)
              // Invalidated object on stack escapes to a field.
              SemaHelper->reportInvalidatedField(IssueExpr,
                                                 FieldEscape->getFieldDecl(),
                                                 Warning.InvalidatedByExpr);
            else if (InvalidatedPVD)
              // Invalidated parameter escapes to a field.
              SemaHelper->reportInvalidatedField(InvalidatedPVD,
                                                 FieldEscape->getFieldDecl(),
                                                 Warning.InvalidatedByExpr);
          } else if (const auto *GlobalEscape =
                         dyn_cast<GlobalEscapeFact>(OEF)) {
            // Invalidated object escapes to global or static storage.
            if (IssueExpr)
              // Invalidated object on stack escapes to global or static
              // storage.
              SemaHelper->reportInvalidatedGlobal(IssueExpr,
                                                  GlobalEscape->getGlobal(),
                                                  Warning.InvalidatedByExpr);
            else if (InvalidatedPVD)
              // Invalidated parameter escapes to global or static storage.
              SemaHelper->reportInvalidatedGlobal(InvalidatedPVD,
                                                  GlobalEscape->getGlobal(),
                                                  Warning.InvalidatedByExpr);
          } else if (isa<ReturnEscapeFact>(OEF)) {
            // FIXME: Diagnose invalidated return escapes separately.
          } else
            llvm_unreachable("Unhandled OriginEscapesFact type");
        } else if (const auto *RetEscape = dyn_cast<ReturnEscapeFact>(OEF))
          // Return stack address.
          SemaHelper->reportUseAfterReturn(
              IssueExpr, RetEscape->getReturnExpr(), MovedExpr);
        else if (const auto *FieldEscape = dyn_cast<FieldEscapeFact>(OEF))
          // Dangling field.
          SemaHelper->reportDanglingField(
              IssueExpr, FieldEscape->getFieldDecl(), MovedExpr, ExpiryLoc);
        else if (const auto *GlobalEscape = dyn_cast<GlobalEscapeFact>(OEF))
          // Global escape.
          SemaHelper->reportDanglingGlobal(IssueExpr, GlobalEscape->getGlobal(),
                                           MovedExpr, ExpiryLoc);
        else
          llvm_unreachable("Unhandled OriginEscapesFact type");
      } else
        llvm_unreachable("Unhandled CausingFact type");
    }
  }

  // Returns declarations that should be annotated with lifetime attributes
  // in order to annotate FDef: the canonical declaration and the earliest
  // redeclarations in each other file. This defines the placement policy for
  // lifetime annotations. Each target is paired with its corresponding warning
  // scope.
  llvm::SmallVector<std::pair<const FunctionDecl *, WarningScope>, 2>
  getTargetDeclsForAttr(const FunctionDecl *FDef) {
    if (!FDef)
      return {};

    assert(FDef->isThisDeclarationADefinition() &&
           "Expected FunctionDecl to be a definition");

    const auto &SM = FDef->getASTContext().getSourceManager();

    auto GetFile = [&SM](const FunctionDecl *FD) {
      return SM.getFileID(SM.getExpansionLoc(FD->getLocation()));
    };

    const FileID DefFile = GetFile(FDef);
    const FunctionDecl *CanonicalDecl = FDef->getCanonicalDecl();
    llvm::SmallVector<std::pair<const FunctionDecl *, WarningScope>, 2> Targets{
        {CanonicalDecl, GetFile(CanonicalDecl) == DefFile
                            ? WarningScope::IntraTU
                            : WarningScope::CrossTU}};

    // Find the earliest redeclaration in each file other than the definition
    // file.
    auto AddCrossTUDecl = [&](const FunctionDecl *FD) {
      FileID File = GetFile(FD);
      if (File == DefFile)
        return;
      for (auto [SeenFD, _] : Targets)
        if (GetFile(SeenFD) == File)
          return;
      Targets.push_back({FD, WarningScope::CrossTU});
    };

    // We iterate in reverse order (from most recent to oldest) to find
    // the first declaration in each file.

    // Store in temporary variable to manually extend lifetime
    auto redecls = llvm::to_vector(FDef->redecls());

    for (const FunctionDecl *Redecl : llvm::reverse(redecls))
      AddCrossTUDecl(Redecl);

    return Targets;
  }

  void suggestWithScopeForParmVar(const ParmVarDecl *PVD,
                                  EscapingTarget EscapeTarget) {
    if (llvm::isa<const VarDecl *>(EscapeTarget))
      return;

    for (auto [Decl, Scope] : getTargetDeclsForAttr(cast<FunctionDecl>(FD))) {
      const auto *ParmToAnnotate =
          Decl->getParamDecl(PVD->getFunctionScopeIndex());
      SemaHelper->suggestLifetimeboundToParmVar(Scope, ParmToAnnotate,
                                                EscapeTarget);
    }
  }

  void suggestWithScopeForImplicitThis(const CXXMethodDecl *MD,
                                       const Expr *EscapeExpr) {
    for (auto [Decl, Scope] : getTargetDeclsForAttr(MD)) {
      SemaHelper->suggestLifetimeboundToImplicitThis(
          Scope, cast<CXXMethodDecl>(Decl), EscapeExpr);
    }
  }

  void suggestAnnotations() {
    if (!SemaHelper)
      return;
    for (auto [Target, EscapeTarget] : AnnotationWarningsMap) {
      if (const auto *PVD = Target.dyn_cast<const ParmVarDecl *>())
        suggestWithScopeForParmVar(PVD, EscapeTarget);
      else if (const auto *MD = Target.dyn_cast<const CXXMethodDecl *>()) {
        if (const auto *EscapeExpr = EscapeTarget.dyn_cast<const Expr *>())
          suggestWithScopeForImplicitThis(MD, EscapeExpr);
        else
          llvm_unreachable("Implicit this can only escape via Expr (return)");
      }
    }
  }

  void reportNoescapeViolations() {
    for (auto [PVD, EscapeTarget] : NoescapeWarningsMap) {
      if (const auto *E = EscapeTarget.dyn_cast<const Expr *>())
        SemaHelper->reportNoescapeViolation(PVD, E);
      else if (const auto *FD = EscapeTarget.dyn_cast<const FieldDecl *>())
        SemaHelper->reportNoescapeViolation(PVD, FD);
      else if (const auto *G = EscapeTarget.dyn_cast<const VarDecl *>())
        SemaHelper->reportNoescapeViolation(PVD, G);
      else
        llvm_unreachable("Unhandled EscapingTarget type");
    }
  }

  void reportThisEscapesToGlobal() {
    if (!SemaHelper)
      return;
    for (auto [Global, Info] : ThisEscapesToGlobalMap)
      SemaHelper->reportThisEscapesToGlobal(Info.Loc, Info.IsField, Global);
    for (auto [PVD, Global] : AnnotatedParamEscapesToGlobalMap)
      SemaHelper->reportAnnotatedParamEscapesToGlobal(PVD, Global);
  }

  // Bans [[clang::lifetime_capture_by(global)]]: the analysis cannot track a
  // borrow captured into global/static storage, so the construct is rejected.
  void checkGlobalCaptureAnnotations() {
    if (!SemaHelper)
      return;
    const auto *Fn = dyn_cast_or_null<FunctionDecl>(FD);
    if (!Fn)
      return;
    for (const ParmVarDecl *PVD : Fn->parameters())
      if (capturesGlobal(PVD))
        SemaHelper->reportGlobalCapture(PVD);
  }

  void reportLifetimeboundViolations() {
    if (!isa<FunctionDecl>(FD))
      return;
    if (ImmortalViolationSubject >= 0)
      SemaHelper->reportImmortalViolation(
          cast<FunctionDecl>(FD), unsigned(ImmortalViolationSubject));
    else if (ImmortalReturnsUntracked)
      SemaHelper->reportImmortalViolation(cast<FunctionDecl>(FD), /*Subject=*/3);
    if (const auto *MD = dyn_cast<CXXMethodDecl>(FD);
        MD && getImplicitObjectParamLifetimeBoundAttr(MD) &&
        !VerifiedLiftimeboundEscapes.contains(MD))
      SemaHelper->reportLifetimeboundViolation(MD);
    for (const ParmVarDecl *PVD : cast<FunctionDecl>(FD)->parameters()) {
      if (!PVD->hasAttr<LifetimeBoundAttr>())
        continue;
      bool isImplicit = PVD->getAttr<LifetimeBoundAttr>()->isImplicit();
      bool Escapes = VerifiedLiftimeboundEscapes.contains(PVD);
      assert((!isImplicit || Escapes || isInStlNamespace(FD)) &&
             "Implicit lifetimebound parameters "
             "should escape through return");
      if (!isImplicit && !Escapes)
        SemaHelper->reportLifetimeboundViolation(PVD);
    }
  }

  // Reports lifetimebound attributes that are placed on a function definition
  // but not on the corresponding declaration.
  void reportMisplacedLifetimebound() {
    const FunctionDecl *FDef = dyn_cast<FunctionDecl>(FD);
    if (!FDef)
      return;

    auto TargetDecls = getTargetDeclsForAttr(FDef);
    // Check if implicit 'this' has lifetimebound on definition but not on
    // declaration.
    if (const auto *MDef = dyn_cast<CXXMethodDecl>(FDef);
        MDef && getDirectImplicitObjectLifetimeBoundAttr(MDef))
      for (auto [Decl, Scope] : TargetDecls) {
        const auto *MDecl = cast<CXXMethodDecl>(Decl);
        if (!getDirectImplicitObjectLifetimeBoundAttr(MDecl))
          SemaHelper->reportMisplacedLifetimebound(Scope, MDef, MDecl);
      }

    // Check each parameter for explicit lifetimebound on definition but not on
    // declaration.
    for (const auto *PDef : FDef->parameters()) {
      const auto *Attr = PDef->getAttr<LifetimeBoundAttr>();
      if (!Attr || Attr->isImplicit())
        continue;
      for (auto [Decl, Scope] : TargetDecls) {
        const auto *PDecl = Decl->getParamDecl(PDef->getFunctionScopeIndex());
        if (!PDecl->hasAttr<LifetimeBoundAttr>())
          SemaHelper->reportMisplacedLifetimebound(Scope, PDef, PDecl);
      }
    }
  }

  void inferAnnotations() {
    for (auto [Target, EscapeTarget] : AnnotationWarningsMap) {
      if (const auto *MD = Target.dyn_cast<const CXXMethodDecl *>()) {
        if (!implicitObjectParamIsLifetimeBound(MD))
          SemaHelper->addLifetimeBoundToImplicitThis(cast<CXXMethodDecl>(MD));
      } else if (const auto *PVD = Target.dyn_cast<const ParmVarDecl *>()) {
        const auto *FD = dyn_cast<FunctionDecl>(PVD->getDeclContext());
        if (!FD)
          continue;
        // Propagates inferred attributes via the most recent declaration to
        // ensure visibility for callers in post-order analysis.
        FD = getDeclWithMergedLifetimeBoundAttrs(FD);
        ParmVarDecl *InferredPVD = const_cast<ParmVarDecl *>(
            FD->getParamDecl(PVD->getFunctionScopeIndex()));
        if (!InferredPVD->hasAttr<LifetimeBoundAttr>())
          InferredPVD->addAttr(
              LifetimeBoundAttr::CreateImplicit(AST, PVD->getLocation()));
      }
    }
  }
};
} // namespace

void runLifetimeChecker(const LoanPropagationAnalysis &LP,
                        const MovedLoansAnalysis &MovedLoans,
                        const LiveOriginsAnalysis &LO, FactManager &FactMgr,
                        AnalysisDeclContext &ADC,
                        LifetimeSafetySemaHelper *SemaHelper) {
  llvm::TimeTraceScope TimeProfile("LifetimeChecker");
  LifetimeChecker Checker(LP, MovedLoans, LO, FactMgr, ADC, SemaHelper);
}

} // namespace clang::lifetimes::internal
