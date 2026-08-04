
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
#include "clang/AST/ParentMap.h"
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

/// True if a borrow rooted at the `$this` placeholder of `MD` is aliased by a
/// mutation of record `MutatedRecord`. `this` designates the *whole* object, so
/// only a mutation of that object itself -- its own type, or a base subobject of
/// it -- can invalidate such a borrow.
///
/// This must NOT be decided by matching the loans directly: a field's loan
/// widens to its enclosing object's `$this` placeholder, so a raw `$this ==
/// $this` match would make every pair of disjoint fields of the same object look
/// aliasing (`grid_.build(asteroids_)`). Mutating a field does not move the
/// enclosing object, so a borrow of `this` survives it; mutating the object (or a
/// base subobject of it) may reallocate the owners it contains, so it does not.
static bool thisBorrowAliasesMutationOf(const CXXMethodDecl *MD,
                                        const CXXRecordDecl *MutatedRecord) {
  if (!MD || !MutatedRecord || !MutatedRecord->hasDefinition())
    return false;
  const CXXRecordDecl *ThisClass = MD->getParent();
  if (!ThisClass || !ThisClass->hasDefinition())
    return false;
  return ThisClass->getCanonicalDecl() == MutatedRecord->getCanonicalDecl() ||
         ThisClass->isDerivedFrom(MutatedRecord);
}

/// True if `RD` is, or has a by-value (possibly inherited / transitive)
/// subobject that is, of type `Target` or a type derived from it AND that same
/// subobject is (or contains) a mutable owner. Recognizes a receiver whose loan
/// roots at an *enclosing* object rather than the receiver subobject itself:
/// `Wrapper w; Base& b = w.d; b.grow()` roots b's loan at `w` (Wrapper), which
/// has a `d` subobject derived from the receiver's static type `Base` and that
/// `Derived d` holds the owner being mutated.
///
/// The owner check must apply to the *matched* subobject, not to `RD` as a
/// whole: an unrelated same-typed subobject must not qualify a plain value-type
/// receiver. E.g. `struct World { SlotMap<Asteroid> pool; Vec2 stray; };` where
/// `a.pos = ...` mutates a `Vec2&` receiver rooted (through a lifetimebound
/// accessor) at `World`. `World` contains a `Vec2` subobject (`stray`) and,
/// separately, a mutable owner (`pool`) -- but `stray` is not `a.pos`, and a
/// `Vec2` assignment cannot reallocate anything. Requiring the *same* subobject
/// to be both is-a-receiver and owner-bearing rejects this false positive while
/// still accepting the Wrapper/Base case. A view/pointer receiver is likewise
/// excluded -- its type is no such by-value subobject of the owner it borrows
/// into. `Visited` cuts cycles.
static bool recordContainsOwnerSubobjectDerivedFrom(
    const CXXRecordDecl *RD, const CXXRecordDecl *Target,
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> &Visited) {
  if (!RD || !RD->hasDefinition() || !Target || !Target->hasDefinition())
    return false;
  if (RD->getCanonicalDecl() == Target->getCanonicalDecl() ||
      RD->isDerivedFrom(Target)) {
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> OwnerVisited;
    return isGslOwnerType(RD) ||
           recordContainsMutableOwner(RD, OwnerVisited);
  }
  if (!Visited.insert(RD->getCanonicalDecl()).second)
    return false;
  for (const CXXBaseSpecifier &B : RD->bases())
    if (recordContainsOwnerSubobjectDerivedFrom(
            B.getType()->getAsCXXRecordDecl(), Target, Visited))
      return true;
  for (const FieldDecl *FD : RD->fields()) {
    QualType FT = FD->getType().getNonReferenceType();
    while (FT->isArrayType())
      FT = FT->getAsArrayTypeUnsafe()->getElementType();
    if (recordContainsOwnerSubobjectDerivedFrom(FT->getAsCXXRecordDecl(), Target,
                                                Visited))
      return true;
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
  /// Parameters annotated [[clang::lifetime_capture_by(X)]] with X *not* naming
  /// `this`, whose borrow nonetheless escapes into the enclosing object (a store
  /// into a field of `this`). The body captures the borrow into `this`, which
  /// the annotation's named capturer does not describe -- and the annotation
  /// suppressed the unannotated-indirection backstop, so the real capture went
  /// unchecked. Keyed by parameter to de-duplicate.
  llvm::DenseSet<const ParmVarDecl *> CaptureByFieldViolations;
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
  /// Mutating expressions already reported as a const-method subversion, to
  /// avoid duplicate warnings when several invalidation facts or loans match the
  /// same mutating call.
  llvm::DenseSet<const Expr *> ReportedConstSubversionExprs;
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
  llvm::DenseSet<std::pair<unsigned, const Stmt *>> ReportedAssumedInval;
  /// (aliased storage, call) pairs already reported as an argument overlap, to
  /// avoid a duplicate when two aliasing reference arguments are symmetric.
  llvm::DenseSet<std::pair<const void *, const Expr *>> ReportedArgOverlap;
  /// Field-store expressions already reported as self-referential.
  llvm::DenseSet<const Expr *> ReportedSelfRefStores;
  /// Assumed-invalidation candidates collected during the fact walk, emitted
  /// after the precise warnings are finalized.
  llvm::SmallVector<std::tuple<LoanID, const Stmt *, const Expr *>>
      PendingAssumedInval;
  const LoanPropagationAnalysis &LoanPropagation;
  const MovedLoansAnalysis &MovedLoans;
  const LiveOriginsAnalysis &LiveOrigins;
  FactManager &FactMgr;
  LifetimeSafetySemaHelper *SemaHelper;
  ASTContext &AST;
  ParentMap &PM;
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
        AST(ADC.getASTContext()), PM(ADC.getParentMap()), FD(ADC.getDecl()) {
    for (const CFGBlock *B : *ADC.getAnalysis<PostOrderCFGView>())
      for (const Fact *F : FactMgr.getFacts(B))
        if (const auto *EF = F->getAs<ExpireFact>())
          checkExpiry(EF);
        else if (const auto *IOF = F->getAs<InvalidateOriginFact>()) {
          checkConstSubversion(IOF);
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
          checkEscapedLostLoan(OEF);
          checkConstSubversionEscape(OEF);
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
    reportCaptureByViolations();
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

  /// Returns true if \p PVD is annotated [[clang::lifetime_capture_by(this)]],
  /// i.e. its capturer list names the implicit object.
  static bool capturesThis(const ParmVarDecl *PVD) {
    if (const auto *A = PVD->getAttr<LifetimeCaptureByAttr>())
      for (int Idx : A->params())
        if (Idx == LifetimeCaptureByAttr::This)
          return true;
    return false;
  }

  /// Returns true if \p PVD is annotated
  /// [[clang::lifetime_capture_by(unknown)]], which documents that the parameter
  /// may be captured by an unspecified location.
  static bool capturesUnknown(const ParmVarDecl *PVD) {
    if (const auto *A = PVD->getAttr<LifetimeCaptureByAttr>())
      for (int Idx : A->params())
        if (Idx == LifetimeCaptureByAttr::Unknown)
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
        const Loan *EL = FactMgr.getLoanMgr().getLoan(LID);
        const AccessPath &AP = EL->getAccessPath();
        switch (AP.getKind()) {
        case AccessPath::Kind::Immortal:
          continue;
        case AccessPath::Kind::NewAllocation:
          // A heap allocation is NOT immortal. `new` / an allocating function
          // gives storage that lives until someone frees it, and whether anyone
          // does is not something the intra-procedural analysis can see: the
          // allocation is handed to the caller, and a `delete` anywhere -- most
          // commonly the destructor of the very object that cached it, or of an
          // owner the result is later given to -- ends its lifetime while callers
          // are still trusting the immortal promise. This mirrors the treatment
          // of a global whose type has a non-trivial destructor below: storage
          // *duration* alone does not make a borrow immortal.
          //
          // A deliberately leaked allocation is genuinely immortal, but nothing
          // in the body distinguishes it from one that is freed later, so it is
          // reported as unprovable rather than accepted.
          ImmortalReturnsUntracked = true;
          continue;
        case AccessPath::Kind::Unknown:
          // Untracked: the analysis cannot prove this borrow is immortal. It may
          // be a local laundered through a borrow-returning call (e.g.
          // string_view::substr yields an Unknown loan even when it views a
          // local). An immortal function must return only provably-immortal
          // storage, so an unverifiable borrow is a violation too. But an Unknown
          // loan from a *construct* expression is a default/empty view
          // (`return {};`) that borrows nothing -- that is genuinely immortal, so
          // only a call-issued untracked borrow is a violation.
          if (isa_and_present<CallExpr>(EL->getIssuingExpr()))
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
          // A global/static variable is immortal -- unless its type has a
          // non-trivial destructor, which frees its storage at static
          // destruction. A borrow of such a global's buffer (e.g. a
          // `std::string_view` of a `std::string` global) is not immortal: a
          // caller keeping it can read freed memory at teardown, and the
          // destruction order is not something the analysis can reason about.
          if (VD && VD->hasGlobalStorage()) {
            QualType GlobalTy = AST.getBaseElementType(VD->getType());
            const CXXRecordDecl *RD = GlobalTy->getAsCXXRecordDecl();
            if (!RD || !RD->hasNonTrivialDestructor())
              continue;
            // Report as "an object the analysis cannot prove is immortal": the
            // storage duration is static but the buffer is freed at teardown.
            ImmortalReturnsUntracked = true;
            continue;
          }
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
        // A noescape parameter forwarded into a callee's
        // [[clang::lifetime_capture_by(this)]] parameter escapes into the
        // (caller-scoped) object -- a violation, anchored at the capturing call.
        if (auto *ThisEsc = dyn_cast<CapturedByThisEscapeFact>(OEF))
          NoescapeWarningsMap.try_emplace(PVD, ThisEsc->getCaptureExpr());
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
      if (const auto *PVD = AP.getAsPlaceholderParam()) {
        // A [[clang::lifetime_capture_by(X)]] parameter promises its borrow is
        // captured by X. If the borrow instead escapes into the enclosing
        // object -- a store into a field of `this` (FieldEscapeFact) -- and X
        // does not name `this`, the body contradicts the annotation. The
        // annotation suppressed the unannotated-indirection backstop, so this
        // real capture into `this` would otherwise go unchecked and the borrow
        // can dangle. Flag it as a capture_by violation. (A capture into a
        // genuine parameter capturer produces no field escape; a truthful
        // capture_by(this) names `this` and is excluded -- it is validated
        // elsewhere, e.g. owner-capture.)
        if (isa<FieldEscapeFact>(OEF) && PVD->hasAttr<LifetimeCaptureByAttr>() &&
            !capturesThis(PVD))
          CaptureByFieldViolations.insert(PVD);
        CheckParam(PVD, /*IsMoved=*/MovedAtEscape.lookup(LID));
      } else if (const auto *MD = AP.getAsPlaceholderThis())
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
  // True if origin `OID` is a borrow *into* an object of record `ObjRD` (a
  // view, or a raw pointer/reference whose pointee is some sub-buffer), as
  // opposed to a pointer *at* the whole object (pointee is `ObjRD` itself,
  // which a field mutation does not dangle, and which is how the object's own
  // `this`/variable origin presents -- so this also excludes the receiver
  // itself from being reported as a borrow into itself).
  bool originBorrowsInto(OriginID OID, const CXXRecordDecl *ObjRD) const {
    const Type *Ty = FactMgr.getOriginMgr().getOrigin(OID).Ty;
    if (!Ty)
      return false;
    QualType QT(Ty, 0);
    if (isGslPointerType(QT))
      return true; // a view (string_view/span/iterator)
    if (!QT->isPointerType() && !QT->isReferenceType())
      return false;
    const CXXRecordDecl *PointeeRD = QT->getPointeeType()->getAsCXXRecordDecl();
    return !ObjRD || !PointeeRD ||
           PointeeRD->getCanonicalDecl() != ObjRD->getCanonicalDecl();
  }

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
      return originBorrowsInto(OID, ObjRD);
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
  /// members there is the normal ownership pattern) -- UNLESS the enclosing type
  /// is a [[gsl::Pointer]] view. A view owns nothing, so its destructor must not
  /// deallocate; a freeing view-destructor is a contract lie that silently turns
  /// every borrow handed into the view (e.g. via aggregate initialization, which
  /// has no constructor parameter to flag) into a dangling alias the caller
  /// cannot see. Verify the body rather than trust the annotation (cf.
  /// immortal-violation / lifetimebound-violation).
  void checkNakedDeallocation(const InvalidateOriginFact *IOF) {
    if (!SemaHelper)
      return;
    if (const auto *DD = dyn_cast_or_null<CXXDestructorDecl>(FD))
      if (const CXXRecordDecl *RD = DD->getParent();
          !RD || !isGslPointerType(AST.getCanonicalTagType(RD)))
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

  /// Soundness check: a member function whose object the analysis trusts as
  /// `const` must not mutate an owner reachable from that object -- doing so
  /// would invalidate borrows a sibling accessor handed out, breaking the
  /// assumption that const methods do not invalidate borrows into the object.
  /// `const` does not protect a pointee/referent reached through a pointer or
  /// reference, so a mutation can slip through any number of laundering forms
  /// (`this->p->m()`, `getPtr()->m()`, `(c?p:q)->m()`, a deducing-this
  /// `self.p->m()`, ...). Rather than match those AST shapes, this is loan-based:
  /// the dataflow already roots the mutating call's receiver loan at the object
  /// it reached (a `$this`/`$self` placeholder, or an `Uninitialized`/field loan
  /// rooted at a member of the object's record). If the invalidated origin holds
  /// such a loan inside a const-trusted method, the mutation reached `this`.
  /// If the current function is a `const`-trusted member function -- a `const`
  /// instance method, or a C++23 deducing-this method whose explicit object
  /// parameter is a `const` *reference* (`this const X& self`; a by-value
  /// explicit object is a copy, so mutating it cannot affect the caller's
  /// object) -- returns its record and, for the deducing-this form, sets `Self`
  /// to the explicit object parameter. Returns null otherwise.
  const CXXRecordDecl *constTrustedSelf(const ParmVarDecl *&Self) const {
    Self = nullptr;
    const auto *MD = dyn_cast_or_null<CXXMethodDecl>(FD);
    if (!MD)
      return nullptr;
    if (MD->isExplicitObjectMemberFunction()) {
      const ParmVarDecl *Obj = MD->getParamDecl(0);
      QualType T = Obj->getType();
      if (!T->isReferenceType() || !T.getNonReferenceType().isConstQualified())
        return nullptr;
      Self = Obj;
    } else if (!MD->isConst()) {
      return nullptr;
    }
    return MD->getParent();
  }

  /// True if any loan held by `OID` at program point `PP` is rooted at the
  /// `const`-trusted object `Record` -- the `$this`/`$self` placeholder, or an
  /// `Uninitialized`/field loan whose decl is a (possibly transitive /
  /// inherited) member of `Record`. `Self` is the deducing-this explicit object
  /// parameter (or null).
  bool originReachesConstObject(OriginID OID, ProgramPoint PP,
                                const CXXRecordDecl *Record,
                                const ParmVarDecl *Self) const {
    for (LoanID L : LoanPropagation.getLoans(OID, PP)) {
      const AccessPath &AP = FactMgr.getLoanMgr().getLoan(L)->getAccessPath();
      if (const CXXMethodDecl *PThis = AP.getAsPlaceholderThis()) {
        if (PThis->getParent()->getCanonicalDecl() == Record->getCanonicalDecl())
          return true;
      } else if (const ParmVarDecl *PParam = AP.getAsPlaceholderParam()) {
        if (PParam == Self)
          return true; // the deducing-this explicit object
      } else {
        const ValueDecl *VD = AP.getAsValueDecl();
        if (!VD)
          VD = AP.getAsUninitialized();
        if (const auto *Field = dyn_cast_or_null<FieldDecl>(VD)) {
          llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
          if (recordReachesField(Record, Field, Visited))
            return true;
        }
      }
    }
    return false;
  }

  /// True if `T` (peeling one pointer/reference) is, or transitively contains, a
  /// mutable owner -- the only thing whose mutation can dangle a borrow a sibling
  /// accessor handed out.
  static bool pointeeIsMutableOwner(QualType T) {
    if (T.isNull())
      return false;
    const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
    return isGslOwnerType(T) ||
           (RD && recordContainsMutableOwner(RD, Visited));
  }

  void checkConstSubversion(const InvalidateOriginFact *IOF) {
    if (!SemaHelper)
      return;
    const Expr *InvExpr = IOF->getInvalidationExpr();
    if (!InvExpr)
      return; // a non-Expr invalidation (e.g. a destructor trigger).
    // The mutation only subverts `const` if the thing mutated is (or transitively
    // contains) a mutable owner -- only then can it invalidate a borrow a sibling
    // accessor handed out. The invalidated receiver's type is the mutated object
    // (peel pointer/reference). A mutation of a plain non-owner pointee (a pimpl
    // whose Impl holds no owner) cannot dangle a view, so it is not flagged.
    QualType RecvTy;
    if (const Type *T = FactMgr.getOriginMgr().getOrigin(IOF->getInvalidatedOrigin()).Ty)
      RecvTy = QualType(T, 0).getNonReferenceType();
    // The receiver origin does not always carry a type (e.g. a member access off
    // a cast result). For a member/operator call the mutated object is the
    // implicit object argument, whose type is always available -- use it.
    if (RecvTy.isNull()) {
      if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(InvExpr))
        RecvTy = MCE->getImplicitObjectArgument()->getType().getNonReferenceType();
      else if (const auto *OCE = dyn_cast<CXXOperatorCallExpr>(InvExpr);
               OCE && OCE->getNumArgs() > 0)
        RecvTy = OCE->getArg(0)->getType().getNonReferenceType();
    }
    if (!RecvTy.isNull() && RecvTy->isPointerType())
      RecvTy = RecvTy->getPointeeType();
    if (!pointeeIsMutableOwner(RecvTy))
      return;
    // The mutation subverts `const` if the mutated origin reaches a const-trusted
    // object: the `this` of a `const` member function, or -- the indirection
    // analogue -- a `const`-reference/pointer parameter (an indirection to a
    // const value the caller trusts will not be mutated behind its back).
    OriginID OID = IOF->getInvalidatedOrigin();
    const ParmVarDecl *Self = nullptr;
    const CXXRecordDecl *Record = constTrustedSelf(Self);
    bool Subverts =
        (Record && originReachesConstObject(OID, IOF, Record, Self)) ||
        originReachesConstParam(OID, IOF);
    if (Subverts && ReportedConstSubversionExprs.insert(InvExpr).second)
      SemaHelper->reportConstMethodIndirectMutation(InvExpr);
  }

  /// True if any loan held by `OID` at `PP` is rooted at a `const`-reference or
  /// `const`-pointer parameter -- an indirection to a const value. Mutating a
  /// mutable owner reached through such a parameter (possible only via a
  /// pointer/smart-pointer member whose const does not reach the pointee, i.e.
  /// shallow const) mutates a value the caller passed as const, behind the
  /// analysis' back -- the parameter analogue of mutating `this` in a `const`
  /// member function. Matched by parameter identity (the placeholder loan a
  /// member access merges in), so a same-typed local or `this` mutated
  /// legitimately does not match. A by-value `const` parameter is a copy and is
  /// excluded.
  bool originReachesConstParam(OriginID OID, ProgramPoint PP) const {
    for (LoanID L : LoanPropagation.getLoans(OID, PP)) {
      const ParmVarDecl *P = FactMgr.getLoanMgr()
                                 .getLoan(L)
                                 ->getAccessPath()
                                 .getAsPlaceholderParam();
      if (!P)
        continue;
      QualType T = P->getType();
      if (T->isReferenceType()
              ? T.getNonReferenceType().isConstQualified()
              : T->isPointerType() && T->getPointeeType().isConstQualified())
        return true;
    }
    return false;
  }

  /// Soundness: a `const`-trusted member function that hands out a *non-const*
  /// pointer/reference into a mutable owner reached from the object -- by
  /// returning it, or storing it to a field/global -- subverts `const`. The
  /// caller can mutate the owner through the escaped indirection, invalidating
  /// borrows a sibling accessor returned, which the analysis assumes a const
  /// member function cannot do. (A `const Buf*`/`const Buf&` return is fine: the
  /// pointee is protected. A by-value view return is fine: not a raw
  /// pointer/reference, so the caller holds no handle into the object.) This is
  /// the escape-site counterpart of `checkConstSubversion`, catching the
  /// subversion at the accessor that produces it rather than at a later mutation.
  void checkConstSubversionEscape(const OriginEscapesFact *OEF) {
    if (!SemaHelper)
      return;
    const ParmVarDecl *Self = nullptr;
    const CXXRecordDecl *Record = constTrustedSelf(Self);
    if (!Record)
      return;
    OriginID OID = OEF->getEscapedOriginID();
    const Type *T = FactMgr.getOriginMgr().getOrigin(OID).Ty;
    if (!T)
      return;
    QualType EscTy(T, 0);
    // Only a raw pointer/reference whose pointee is non-const is a handle the
    // caller can mutate through.
    QualType Pointee;
    if (EscTy->isPointerType())
      Pointee = EscTy->getPointeeType();
    else if (EscTy->isReferenceType())
      Pointee = EscTy.getNonReferenceType();
    else
      return;
    if (Pointee.isConstQualified() || !pointeeIsMutableOwner(Pointee))
      return;
    if (!originReachesConstObject(OID, OEF, Record, Self))
      return;
    const Expr *E = nullptr;
    if (const auto *RE = dyn_cast<ReturnEscapeFact>(OEF))
      E = RE->getReturnExpr();
    if (!E)
      return;
    if (ReportedConstSubversionExprs.insert(E).second)
      SemaHelper->reportConstMethodIndirectEscape(E);
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
    // For a loan-gated invalidation (emitted for a receiver whose static type
    // did not reveal an owner -- e.g. a non-const call through a base reference
    // `Base& b = d; b.grow();`), confirm via the loans the receiver actually
    // carries: act only if the receiver DENOTES a mutable owner. That means a
    // loan it holds points at an object that is-a the receiver's own type (the
    // receiver is that object, possibly viewed as a base) AND that object is (or
    // contains) a mutable owner. This is what the receiver refers to (robust to
    // references/pointers/ternaries the static type cannot express), while
    // excluding a sub-object receiver that merely holds a borrow into some
    // containing owner (`a.pos = ...` where `a` is a reference into a container).
    // The loans the invalidation actually matches against. Normally every loan
    // the mutated origin carries; for a loan-gated invalidation, only those that
    // pass the "denotes a mutable owner" test below. The distinction matters
    // because a member access inherits its enclosing object's loan (`this->b`
    // carries `$this`), so matching against every loan would let a mutation of one
    // field invalidate borrows of a disjoint sibling.
    llvm::SmallVector<LoanID, 8> MatchLoans;
    if (IOF->requiresOwnerLoanTarget()) {
      const Type *RecvT =
          FactMgr.getOriginMgr().getOrigin(IOF->getInvalidatedOrigin()).Ty;
      QualType RecvQT = RecvT ? QualType(RecvT, 0).getNonReferenceType()
                              : QualType();
      if (!RecvQT.isNull() && RecvQT->isPointerType())
        RecvQT = RecvQT->getPointeeType();
      const CXXRecordDecl *RecvRD =
          RecvQT.isNull() ? nullptr : RecvQT->getAsCXXRecordDecl();
      for (LoanID InvalidID : DirectlyInvalidatedLoans) {
        const AccessPath &AP =
            FactMgr.getLoanMgr().getLoan(InvalidID)->getAccessPath();
        const CXXRecordDecl *RT = invalidatedObjectRecord(AP);
        if (!RT || !RecvRD)
          continue;
        bool IsA = RT->getCanonicalDecl() == RecvRD->getCanonicalDecl() ||
                   (RT->hasDefinition() && RecvRD->hasDefinition() &&
                    RT->isDerivedFrom(RecvRD));
        // `IsA`: the loan's record is (a base of) the receiver -- the receiver
        // is that object; it denotes an owner if that object is (or contains) a
        // mutable owner. Otherwise the loan may root at an *enclosing* object
        // (`Wrapper w; Base& b = w.d; b.grow()` roots b's loan at `w`); the
        // receiver still denotes an owner if that record has a by-value
        // subobject derived from the receiver's static type that is *itself* (or
        // contains) a mutable owner. Requiring the matched subobject to be the
        // owner-bearing one (not merely any same-typed subobject coexisting with
        // an unrelated owner) avoids flagging a plain value-type receiver like
        // `a.pos = ...` (`Vec2&`) merely because the enclosing record happens to
        // hold both a stray `Vec2` and a container. A view merely borrowing into
        // an owner is excluded -- its type is no such by-value subobject.
        llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited, Visited2;
        bool DirectOwner =
            IsA && (isGslOwnerType(RT) || recordContainsMutableOwner(RT, Visited));
        // The enclosing-object fallback is accepted only for a receiver
        // (ReachableOwner), not for the dynamic-dispatch argument case
        // (DenotedOwner) -- see OwnerLoanGate.
        bool AllowEnclosing =
            IOF->getOwnerLoanGate() == OwnerLoanGate::ReachableOwner;
        if (DirectOwner ||
            (AllowEnclosing &&
             recordContainsOwnerSubobjectDerivedFrom(RT, RecvRD, Visited2)))
          MatchLoans.push_back(InvalidID);
      }
      if (MatchLoans.empty())
        return;
    } else {
      llvm::append_range(MatchLoans, DirectlyInvalidatedLoans);
    }
    auto IsInvalidated = [&](const Loan *L) {
      for (LoanID InvalidID : MatchLoans) {
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
      // Among this origin's invalidated loans, prefer one with a precise anchor
      // (an issuing expression or a placeholder parameter) so the diagnostic
      // points at the borrow. Only if none exists fall back to the `$this`
      // placeholder loan (a borrow laundered through a lifetimebound accessor of
      // `this`, which carries neither anchor), reported at the use that keeps it
      // live. The fallback applies only to a borrow *into* the object: the
      // object's own `this` origin is a pointer *at* the whole object and is
      // live throughout every method, so reporting it would fire at each
      // self-mutation.
      LoanID ReportLoan;
      bool HaveReport = false;
      const Expr *FallbackUse = nullptr;
      for (LoanID LiveLoanID : LoanPropagation.getLoans(OID, IOF)) {
        const Loan *L = FactMgr.getLoanMgr().getLoan(LiveLoanID);
        if (!IsInvalidated(L))
          continue;
        if (L->getIssuingExpr() || L->getAccessPath().getAsPlaceholderParam()) {
          // Precise anchor: prefer it and stop looking.
          ReportLoan = LiveLoanID;
          HaveReport = true;
          FallbackUse = nullptr;
          break;
        }
        if (!HaveReport &&
            originBorrowsInto(OID,
                              invalidatedObjectRecord(L->getAccessPath())))
          if (const auto *UF =
                  LiveInfo.CausingFact.dyn_cast<const UseFact *>()) {
            ReportLoan = LiveLoanID;
            HaveReport = true;
            FallbackUse = UF->getUseExpr();
          }
      }
      if (!HaveReport)
        continue;
      // Report each invalidated view (origin) at most once per operation: a
      // multi-level field borrow (`o.inner.s`) carries both the field loan
      // and the enclosing-object loan, but it is a single captured value.
      if (ReportedAssumedInval.insert({ReportLoan.Value, IOF->getInvalidationStmt()})
              .second)
        PendingAssumedInval.push_back(
            {ReportLoan, IOF->getInvalidationStmt(), FallbackUse});
    }
  }

  /// Emits the collected assumed-invalidation warnings, skipping any borrow
  /// that was already reported as a known (precise) invalidation.
  void issueAssumedInvalidations() {
    if (!SemaHelper)
      return;
    for (auto &[LID, OperationStmt, FallbackUse] : PendingAssumedInval) {
      // If this borrow is already reported as a known invalidation, the
      // lower-confidence assumed warning would be redundant.
      auto It = FinalWarningsMap.find(LID);
      if (It != FinalWarningsMap.end() && It->second.InvalidatedByExpr)
        continue;
      const Loan *L = FactMgr.getLoanMgr().getLoan(LID);
      if (const Expr *IssueExpr = L->getIssuingExpr())
        SemaHelper->reportAssumedInvalidation(IssueExpr, OperationStmt);
      else if (const ParmVarDecl *PVD =
                   L->getAccessPath().getAsPlaceholderParam())
        SemaHelper->reportAssumedInvalidation(PVD, OperationStmt);
      else if (FallbackUse)
        // The `$this` placeholder loan (laundered through a lifetimebound
        // accessor of `this`): anchor at the use that keeps the borrow live.
        SemaHelper->reportAssumedInvalidation(FallbackUse, OperationStmt);
      else if (const CXXMethodDecl *MD =
                   L->getAccessPath().getAsPlaceholderThis())
        // The `$this` placeholder loan with no use to anchor at: `this` itself
        // is the aliasing argument (argument-overlap), so anchor at the method
        // whose implicit object parameter it stands for.
        SemaHelper->reportAssumedInvalidation(MD, OperationStmt);
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

  /// Soundness: like checkLostLoan, but for an origin that *escapes* the function
  /// (via return, or a store to a field/global) carrying an Unknown loan -- an
  /// untracked borrow (e.g. a non-lifetimebound view returned by
  /// std::string_view::substr). checkLostLoan only fires on a local *use*; an
  /// Unknown loan that is returned or stored without a local use would otherwise
  /// leave the function silently, defeating the annotation checks downstream
  /// (e.g. a [[clang::noescape]] parameter laundered through substr escapes via
  /// return without a noescape violation). An immortal return is reported more
  /// specifically as an immortal violation (see checkAnnotations), so it is
  /// excluded here to avoid a duplicate.
  void checkEscapedLostLoan(const OriginEscapesFact *OEF) {
    if (!SemaHelper)
      return;
    if (isa<ReturnEscapeFact>(OEF) && FD && FD->hasAttr<LifetimeImmortalAttr>())
      return;
    for (LoanID LID : LoanPropagation.getLoans(OEF->getEscapedOriginID(), OEF)) {
      const Loan *L = FactMgr.getLoanMgr().getLoan(LID);
      if (!L->getAccessPath().isUnknown())
        continue;
      // Only an Unknown loan from a borrow-returning *call* whose result was not
      // tracked (e.g. std::string_view::substr) -- that is a borrow the analysis
      // lost. An Unknown loan issued by a construct expression is a default/empty
      // view (`return {};`), which borrows nothing and is safe to escape; do not
      // flag it.
      const Expr *Issuing = L->getIssuingExpr();
      if (!isa_and_present<CallExpr>(Issuing))
        continue;
      // Anchor at the underlying borrow expression: the return operand (stripped
      // of the implicit owner->view conversion) when returning, else the call
      // that issued the untracked loan.
      const Expr *Anchor = Issuing;
      if (const auto *RE = dyn_cast<ReturnEscapeFact>(OEF))
        if (const Expr *Ret = RE->getReturnExpr())
          Anchor = Ret->IgnoreImplicit();
      if (!Anchor)
        continue;
      if (!ReportedLostLoanLocs.insert(Anchor->getExprLoc()).second)
        return;
      SemaHelper->reportLostLoan(Anchor);
      return;
    }
  }

  /// Soundness: a pointer/reference/view that borrows from a mutable global or
  /// static owner (e.g. `int *p = &g[0];`, `p = g.data();`, `string_view sv =
  /// g_str;`, `std::string& f(){ return g_s; }`, `W* pw(){ return &g; }`) aliases
  /// that owner invisibly to the caller: a mutation of the owner from anywhere --
  /// another function or translation unit the intra-procedural analysis cannot
  /// see -- can invalidate a view derived from it, and the borrow can dangle.
  /// The borrow surfaces as a loan rooted at the global, so this loan-based pass
  /// flags every such borrow (raw pointer/reference and GSL view forms) uniformly
  /// once the global is, or transitively contains, a mutable owner.
  ///
  /// The one permitted interaction with a mutable global is a method call on it
  /// (`global.method()`); that receiver is exempted at the use site (see
  /// `isStableGlobalMethodReceiver`), and a borrow the method *returns* is checked
  /// at its own use/escape.
  ///
  /// `OID` is the borrowing value's origin; `Loc`/`Range` anchor the diagnostic.
  /// `ValueTyHint`, when non-null, is the type of the borrow the caller receives
  /// (e.g. a function's return type) -- used so a reference/pointer RETURN of the
  /// whole owner (`std::string& f(){ return g_s; }`) is recognized, where the
  /// escaping origin is typed as the owner value rather than a reference.
  /// `EscapeRoute` is set when the borrow outlives this function invocation (it
  /// is stored into global/static storage or a field, or returned to the
  /// caller); `std::nullopt` means the borrow is only used locally.
  void flagBorrowFromMutableGlobal(
      OriginID OID, ProgramPoint PP, SourceLocation Loc, SourceRange Range,
      QualType ValueTyHint = QualType(),
      std::optional<GlobalDtorOrderRoute> EscapeRoute = std::nullopt) {
    if (!SemaHelper || Loc.isInvalid())
      return;
    // A raw pointer/reference value, or a gsl::Pointer view.
    QualType ValueTy = ValueTyHint;
    if (ValueTy.isNull()) {
      const Type *Ty = FactMgr.getOriginMgr().getOrigin(OID).Ty;
      if (!Ty)
        return;
      ValueTy = QualType(Ty, 0);
    }
    if (!ValueTy->isPointerType() && !ValueTy->isReferenceType() &&
        !isGslPointerType(ValueTy))
      return;
    for (LoanID L : LoanPropagation.getLoans(OID, PP)) {
      const AccessPath &AP = FactMgr.getLoanMgr().getLoan(L)->getAccessPath();
      const auto *VD = dyn_cast_or_null<VarDecl>(AP.getAsValueDecl());
      if (!VD || !VD->hasGlobalStorage())
        continue;
      // Peel array dimensions: a global array of owners (`std::string g[4]`)
      // owns reallocatable storage per element, but the loan roots at the array
      // variable whose own type is the array, not an owner. Test the element
      // type for owner-ness.
      QualType GlobalTy = AST.getBaseElementType(VD->getType());
      // The global must itself be an owner, or a record that transitively
      // contains one (`struct W { std::string s; } g_w;`). A global with no owner
      // anywhere (a plain scalar, `int g_int`, `int g_arr[8]`) has no
      // reallocatable buffer and no aliasing/teardown hazard, so it is not
      // flagged.
      bool DirectOwner = isGslOwnerType(GlobalTy);
      bool WrapsOwner = false;
      if (!DirectOwner)
        if (const CXXRecordDecl *RD = GlobalTy->getAsCXXRecordDecl()) {
          llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
          WrapsOwner = recordContainsMutableOwner(RD, Visited);
        }
      if (!DirectOwner && !WrapsOwner)
        continue;
      if (GlobalTy.isConstQualified()) {
        // A const global owner cannot be mutated, so it has no aliasing hazard.
        // But its buffer is still freed by its (non-trivial) destructor at
        // static destruction, and the destruction order across translation units
        // is not something the intra-procedural analysis can reason about. So a
        // borrow that OUTLIVES this function -- stored into other global/static
        // storage or a field, or returned to the caller -- can be read after the
        // buffer is freed, by a global destroyed later or by a caller running at
        // teardown.
        //
        // A purely local use is not flagged. It is safe during normal execution
        // (the const global outlives the call), and unsafe only if this function
        // itself runs during static destruction -- which is whole-program
        // reachability the intra-procedural analysis cannot decide. Keying it on
        // "is a destructor" was tried and is a bad trade: it flags the common safe
        // case (a destructor of a purely *local* object) while still missing the
        // real bug behind one level of indirection (a destructor calling a helper
        // that reads the global). So a local read at teardown remains a known gap.
        const CXXRecordDecl *RD = GlobalTy->getAsCXXRecordDecl();
        if (!EscapeRoute || !RD || !RD->hasNonTrivialDestructor())
          continue;
        if (!ReportedMutableGlobalLocs.insert(Loc).second)
          return;
        SemaHelper->reportGlobalDtorOrder(Loc, ValueTy, Range, *EscapeRoute);
        return;
      }
      // Any pointer/reference/view borrow of a global that is or contains a
      // mutable owner is flagged: it aliases that owner invisibly to the caller,
      // so a mutation of the owner elsewhere (another function or TU) can
      // invalidate a view derived from it, and the borrow itself can dangle.
      // This is intentionally broad -- the one permitted interaction with a
      // mutable global is a method call on it (`g.method()`), exempted earlier at
      // the use site (`isStableGlobalMethodReceiver`); a borrow the method
      // *returns* is checked at its own use/escape. (A reference to a stable
      // non-owner scalar member, `int& g.counter`, does not itself dangle, but it
      // is indistinguishable here from a raw pointer into an owner's buffer
      // returned by an accessor (`g.data()`), so it is flagged too; annotate the
      // accessor [[clang::lifetime_immortal]] if the contract permits it.)
      if (!ReportedMutableGlobalLocs.insert(Loc).second)
        return;
      SemaHelper->reportViewOnMutableGlobal(Loc, ValueTy, Range);
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
    const Expr *Use = UF->getUseExpr();
    if (!Use)
      return;
    // Reading a by-value owner (`return g_s;` -- a copy) is not a borrow: the
    // caller gets a copy, and the read is materialized as a `const Owner&`
    // copy-constructor binding that would otherwise look like a reference borrow.
    // Skip a use that designates a by-value owner object; a view / reference /
    // pointer *derived* from the owner is flagged at its own use or escape.
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Use->IgnoreParenCasts()))
      if (const ValueDecl *D = DRE->getDecl();
          D && !D->getType()->isReferenceType() &&
          !D->getType()->isPointerType() && isGslOwnerType(D->getType()))
        return;
    // The one permitted interaction with a mutable global is a method call on
    // the global itself (`g.method()` / `g.owner.append()`). The receiver is a
    // transient access, not a borrow the caller keeps, so do not flag it -- any
    // borrow the method *returns* is checked at its own use/escape. This applies
    // only when the receiver *is* the global owner; a separate borrow of it (a
    // local view `sv` bound to `g`, used as `sv.front()`) is still flagged.
    if (isMutableGlobalMethodReceiver(Use))
      return;
    if (const OriginNode *OL = UF->getUsedOrigins())
      flagBorrowFromMutableGlobal(OL->getOriginID(), UF, Use->getExprLoc(),
                                  Use->getSourceRange());
  }

  /// True if `Use` is the implicit object argument (receiver) of a member call
  /// whose receiver *is* a mutable-owner global itself -- the permitted
  /// `global.method()` form (`g.mutate()`, `g.owner.append()`, `g_arr[i].m()`).
  /// The receiver must be a direct designation (variable / member / subscript /
  /// deref chain, no selecting or borrow-producing node) ROOTED at a mutable
  /// global owner. A receiver that is a separate borrow of the global -- a local
  /// view `sv` bound to `g`, where `sv.front()` reads the dangling buffer -- is
  /// NOT exempt: its root is a local, not the global owner. Likewise a selecting
  /// receiver (`(c ? g1 : g2).method()`) computes a borrow and is not exempt.
  bool isMutableGlobalMethodReceiver(const Expr *Use) {
    // Climb past value-preserving wrappers (implicit casts / parens /
    // materialized temporaries) that the front end inserts between the receiver
    // expression and the MemberExpr -- e.g. a NoOp cast adding `const` for a
    // const method (`g` -> `const W` lvalue -> `g.method`). Without this, the
    // `DeclRefExpr` receiver's immediate parent is the cast, not the call.
    const Stmt *Cur = Use;
    const Stmt *Parent = PM.getParent(Cur);
    while (Parent && (isa<ImplicitCastExpr>(Parent) || isa<ParenExpr>(Parent) ||
                      isa<MaterializeTemporaryExpr>(Parent))) {
      Cur = Parent;
      Parent = PM.getParent(Cur);
    }
    // The receiver must be the base of the MemberExpr that is the callee of a
    // member call -- i.e. the implicit object argument.
    const auto *ME = dyn_cast_or_null<MemberExpr>(Parent);
    if (!ME)
      return false;
    const auto *MCE = dyn_cast_or_null<CXXMemberCallExpr>(PM.getParent(ME));
    if (!MCE || MCE->getCallee()->IgnoreParens() != ME)
      return false;
    // The receiver designation must root at the global owner itself.
    return isMutableOwnerGlobal(
        dyn_cast_or_null<VarDecl>(stableDesignationRoot(Use)));
  }

  /// If `E` is a statically-known designation of storage -- a chain of variable
  /// reference / member access / array subscript / pointer dereference, with no
  /// selecting or borrow-producing node (conditional, comma, call) -- return the
  /// entity its leaf names (a `VarDecl`/`FieldDecl`); otherwise null. `this` is a
  /// designation but names no decl, so it yields null (never a global match).
  static const ValueDecl *stableDesignationRoot(const Expr *E) {
    E = E->IgnoreParenCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return DRE->getDecl();
    if (const auto *ME = dyn_cast<MemberExpr>(E))
      return stableDesignationRoot(ME->getBase());
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
      return stableDesignationRoot(ASE->getBase());
    if (const auto *UO = dyn_cast<UnaryOperator>(E))
      return UO->getOpcode() == UO_Deref ? stableDesignationRoot(UO->getSubExpr())
                                         : nullptr;
    return nullptr;
  }

  /// True if `VD` is a non-const global/static variable that is, or transitively
  /// contains, a mutable owner -- the subject of the borrow-from-mutable-global
  /// rule (array dimensions peeled; a const element/object is excluded).
  bool isMutableOwnerGlobal(const VarDecl *VD) {
    if (!VD || !VD->hasGlobalStorage())
      return false;
    QualType GlobalTy = AST.getBaseElementType(VD->getType());
    if (GlobalTy.isConstQualified())
      return false;
    if (isGslOwnerType(GlobalTy))
      return true;
    if (const CXXRecordDecl *RD = GlobalTy->getAsCXXRecordDecl()) {
      llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
      return recordContainsMutableOwner(RD, Visited);
    }
    return false;
  }

  /// The escape forms. A return -- `int *f() { return &g[0]; }`, or a view
  /// return `std::string_view f() { return g_table[i]; }` (the latter wrapped in
  /// an implicit owner->view conversion, stripped for a precise location). Also
  /// a store into a field or global: a view cached into the member of a
  /// [[gsl::Owner]], or assigned to global storage, borrows from the mutable
  /// global just the same -- and there is no later *use* of it in this function
  /// (the dangling read is elsewhere, through the owner's opaque member), so the
  /// escape is the only point the borrow is visible. Anchored at the
  /// field/global declaration since the store expression is not available at the
  /// function-exit escape point.
  void checkBorrowFromMutableGlobal(const OriginEscapesFact *OEF) {
    OriginID OID = OEF->getEscapedOriginID();
    if (const auto *RE = dyn_cast<ReturnEscapeFact>(OEF)) {
      const Expr *Ret = RE->getReturnExpr();
      if (Ret)
        Ret = Ret->IgnoreImplicit();
      // Use the function's return type as the borrow's value type: a
      // reference/pointer return of the whole owner (`std::string& f(){ return
      // g_s; }`) escapes the owner's own origin (typed as the owner value), so
      // the return type is what tells us the caller receives a reference. A
      // by-value owner return copies and does not escape the owner origin, so it
      // never reaches here.
      QualType RetTy;
      if (const auto *Fn = dyn_cast_or_null<FunctionDecl>(FD))
        RetTy = Fn->getReturnType();
      // A [[clang::lifetime_immortal]] function returning such a borrow is
      // reported more specifically by the immortal body verifier ("an object the
      // analysis cannot prove is immortal", see checkAnnotations), so skip the
      // return route here to avoid a duplicate.
      std::optional<GlobalDtorOrderRoute> Route = GlobalDtorOrderRoute::Returned;
      if (FD && FD->hasAttr<LifetimeImmortalAttr>())
        Route = std::nullopt;
      flagBorrowFromMutableGlobal(
          OID, OEF, Ret ? Ret->getExprLoc() : SourceLocation(),
          Ret ? Ret->getSourceRange() : SourceRange(), RetTy, Route);
    } else if (const auto *FE = dyn_cast<FieldEscapeFact>(OEF)) {
      const FieldDecl *FD = FE->getFieldDecl();
      flagBorrowFromMutableGlobal(OID, OEF, FD->getLocation(),
                                  FD->getSourceRange(), QualType(),
                                  GlobalDtorOrderRoute::EscapesToGlobal);
    } else if (const auto *GE = dyn_cast<GlobalEscapeFact>(OEF)) {
      const VarDecl *VD = GE->getGlobal();
      flagBorrowFromMutableGlobal(OID, OEF, VD->getLocation(),
                                  VD->getSourceRange(), QualType(),
                                  GlobalDtorOrderRoute::EscapesToGlobal);
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
        SemaHelper->reportOwnerOfIndirection(D, UCF->getReportType());
      else
        SemaHelper->reportOwnerOfIndirection(E, UCF->getReportType());
      break;
    case UntrackedConstructReason::PointerOfIndirection:
      if (D)
        SemaHelper->reportPointerOfIndirection(D, UCF->getReportType());
      else
        SemaHelper->reportPointerOfIndirection(E, UCF->getReportType());
      break;
    case UntrackedConstructReason::Exception:
      if (ReportedUntrackedLocs.insert(UCF->getConstructLoc()).second)
        SemaHelper->reportException(UCF->getConstructLoc());
      break;
    case UntrackedConstructReason::InlineAsm:
      if (ReportedUntrackedLocs.insert(UCF->getConstructLoc()).second)
        SemaHelper->reportInlineAsm(UCF->getConstructLoc());
      break;
    case UntrackedConstructReason::SetjmpLongjmp:
      SemaHelper->reportSetjmpLongjmp(E->getExprLoc());
      break;
    case UntrackedConstructReason::Coroutine:
      if (ReportedUntrackedLocs.insert(UCF->getConstructLoc()).second)
        SemaHelper->reportCoroutine(UCF->getConstructLoc());
      break;
    case UntrackedConstructReason::ViewOnMutableGlobal:
      // Currently the view-on-mutable-global diagnostic is emitted loan-based
      // (checkBorrowFromMutableGlobal), not via this fact; kept for completeness.
      if (E)
        SemaHelper->reportViewOnMutableGlobal(E->getExprLoc(), E->getType(),
                                              E->getSourceRange());
      break;
    case UntrackedConstructReason::ConstMethodIndirectMutation:
      SemaHelper->reportConstMethodIndirectMutation(E);
      break;
    case UntrackedConstructReason::MultiLevelIndirectionExpr:
      SemaHelper->reportMultiLevelIndirection(E);
      break;
    case UntrackedConstructReason::LambdaRefCaptureIndirection:
      SemaHelper->reportMultiLevelIndirectionCapture(E);
      break;
    case UntrackedConstructReason::ArrayOfIndirectionDecay:
      SemaHelper->reportArrayOfIndirectionDecay(E);
      break;
    case UntrackedConstructReason::UnsupportedStoreDestination:
      SemaHelper->reportUnsupportedStoreDestination(E);
      break;
    case UntrackedConstructReason::UnmodeledExpr:
      if (ReportedUntrackedLocs.insert(E->getExprLoc()).second)
        SemaHelper->reportUnmodeledExpr(E);
      break;
    case UntrackedConstructReason::Union:
      if (ReportedUntrackedLocs.insert(E->getExprLoc()).second)
        SemaHelper->reportUnion(E);
      break;
    case UntrackedConstructReason::ReinterpretCast:
      if (ReportedUntrackedLocs.insert(E->getExprLoc()).second)
        SemaHelper->reportReinterpretCast(E);
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
    // Gather the loans across the mutating argument and its pointee chain: for
    // a gsl::Pointer receiver the borrow into the aliased owner lives on the
    // pointee origin, not on the wrapper's own origin.
    llvm::SmallVector<LoanID, 8> Mutating;
    for (OriginID MO : AOF->getMutatingOrigins())
      for (LoanID ML : LoanPropagation.getLoans(MO, AOF))
        Mutating.push_back(ML);
    if (Mutating.empty())
      return;
    const Expr *Op = AOF->getOperationExpr();
    // The precise record being mutated -- the actual subobject (e.g. `Grid` for
    // `world.grid_.build(...)`), taken from the argument's static type, not from
    // the mutating loan (which may widen to the enclosing object's `$this`
    // placeholder and over-match disjoint sibling fields).
    const CXXRecordDecl *MutatedRecord = AOF->getMutatedRecord();
    for (OriginID BO : AOF->getBorrowOrigins()) {
      LoanSet BLoans = LoanPropagation.getLoans(BO, AOF);
      // Prefer an aliasing loan with a precise anchor (an issuing expression or
      // a placeholder parameter) so the diagnostic points at the borrow itself.
      // Only if none exists fall back to the `$this` placeholder loan, which
      // carries neither anchor but is reportable at the method whose implicit
      // object it stands for -- without that fallback, passing `this` as an
      // aliasing argument (`f(this, this)`, `this->m(this)`) is detected here
      // and then silently dropped.
      LoanID ReportLoan;
      bool HaveReport = false;
      for (LoanID BL : BLoans) {
        const Loan *BLoan = FactMgr.getLoanMgr().getLoan(BL);
        const AccessPath &BAP = BLoan->getAccessPath();
        bool HasPreciseAnchor =
            BLoan->getIssuingExpr() || BAP.getAsPlaceholderParam();
        const CXXMethodDecl *BorrowsThis = BAP.getAsPlaceholderThis();
        // Skip loans with no reportable anchor at all; they would emit nothing.
        if (!HasPreciseAnchor && !BorrowsThis)
          continue;
        bool Aliases;
        if (BorrowsThis) {
          // A borrow of the whole object: only a mutation of that object (or of
          // a base subobject of it) aliases it. Deciding this by loan identity
          // would over-match disjoint fields, whose loans widen to the same
          // `$this` root.
          Aliases = thisBorrowAliasesMutationOf(BorrowsThis, MutatedRecord);
        } else {
          // The borrow aliases the mutated argument if it borrows the same
          // storage, OR a SUBOBJECT of it: mutating an object (`a` / the
          // receiver `this`) may reallocate any owner field it (transitively)
          // contains, dangling a field-rooted borrow into it. So `f(a, a.b)` /
          // `obj.m(this->buf)` overlap, while disjoint subobjects (`f(a.b,
          // a.c)`) do not -- `c` is not a member of `b`'s type. (Field loans are
          // instance-insensitive: the same accepted over-approximation as the
          // invalidation check.)
          Aliases = isFieldBorrowOf(BLoan, MutatedRecord);
          for (LoanID ML : Mutating) {
            if (Aliases)
              break;
            const AccessPath &MAP =
                FactMgr.getLoanMgr().getLoan(ML)->getAccessPath();
            if (MAP == BAP)
              Aliases = true;
          }
        }
        if (!Aliases)
          continue;
        ReportLoan = BL;
        HaveReport = true;
        if (HasPreciseAnchor)
          break; // precise anchor: prefer it and stop looking
      }
      if (!HaveReport)
        continue;
      const AccessPath &RAP =
          FactMgr.getLoanMgr().getLoan(ReportLoan)->getAccessPath();
      // Two aliasing reference arguments produce symmetric facts (a->[b] and
      // b->[a]); they describe the same hazard, so de-duplicate by (aliased
      // storage, call). For a `this` argument the storage is the implicit
      // object, keyed by the method the placeholder stands for.
      const void *Storage = RAP.getAsValueDecl();
      if (!Storage)
        Storage = RAP.getAsPlaceholderThis();
      if (Storage && !ReportedArgOverlap.insert({Storage, Op}).second)
        continue;
      // Reuse the assumed-invalidation reporting (the operation is the call).
      if (ReportedAssumedInval.insert({ReportLoan.Value, Op}).second)
        PendingAssumedInval.push_back({ReportLoan, Op, /*FallbackUse=*/nullptr});
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
      if (isGslOwnerOfIndirection(PVD->getType().getNonReferenceType(),
                                  FactMgr.getUnknownOwnershipCache())) {
        if (ReportedUntrackedDecls.insert(PVD).second)
          SemaHelper->reportOwnerOfIndirection(PVD);
        continue;
      }
      // Likewise a gsl::Pointer view parameter whose pointee is an indirection
      // (e.g. std::span<int*>); the inner pointees are not tracked.
      if (isGslPointerOfIndirection(PVD->getType().getNonReferenceType(),
                                    FactMgr.getUnknownOwnershipCache())) {
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
    // (including a conversion operator) whose return value is a borrow-carrying
    // value must declare where that borrow comes from -- otherwise a (frequently
    // implicit, e.g. `int* p = obj;`) call site cannot track it. This covers a
    // pointer/reference/gsl::Pointer return and, more generally, any return type
    // that can hold a borrow -- in particular a lambda's operator() returning a
    // closure that captures by reference (the returned closure depends on the
    // implicit object's captures, but is not a pointer-like type). Require
    // [[clang::lifetimebound]] on the implicit object (or on a parameter the
    // borrow may come from instead) or [[clang::lifetime_immortal]] on the
    // function. Standard-library accessors are modeled via GSL recognition and
    // are exempt.
    const auto *MD = dyn_cast<CXXMethodDecl>(Fn);
    if (!MD || !MD->isInstance() || isInStlNamespace(MD->getParent()))
      return;
    QualType RetTy = MD->getReturnType();
    if (!isPointerLikeType(RetTy) && !RetTy->isReferenceType() &&
        !FactMgr.getOriginMgr().hasOrigins(RetTy))
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

    // The function's RETURN TYPE is subject to the same single-indirection rule:
    // returning a reference/pointer to an indirection (e.g. 'std::string_view&')
    // is a double indirection the analysis cannot model -- and a store *through*
    // such a returned reference (`obj.ref() = borrow;`) silently drops the
    // borrow. (A view returned BY VALUE is a single level and stays fine.)
    if (const auto *Fn = dyn_cast_or_null<FunctionDecl>(FD))
      if (OM.getIndirectionDepth(Fn->getReturnType()) > 1)
        SemaHelper->reportMultiLevelIndirectionReturn(Fn);
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
          else
            // A loan with no natural anchor -- the `$this` placeholder loan from
            // a lifetimebound accessor of `this`, invalidated by a self-mutation
            // -- is anchored at the use that keeps the borrow live.
            SemaHelper->reportUseAfterInvalidation(
                UF->getUseExpr(), UF->getUseExpr(), Warning.InvalidatedByExpr);

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

  void reportCaptureByViolations() {
    if (!SemaHelper)
      return;
    for (const ParmVarDecl *PVD : CaptureByFieldViolations)
      SemaHelper->reportCaptureByViolation(PVD);
  }

  // Bans [[clang::lifetime_capture_by(global)]] and
  // [[clang::lifetime_capture_by(unknown)]]: the analysis cannot track a borrow
  // captured into global/static storage or an unspecified location, so the
  // construct is rejected.
  void checkGlobalCaptureAnnotations() {
    if (!SemaHelper)
      return;
    const auto *Fn = dyn_cast_or_null<FunctionDecl>(FD);
    if (!Fn)
      return;
    for (const ParmVarDecl *PVD : Fn->parameters())
      if (capturesGlobal(PVD))
        SemaHelper->reportGlobalCapture(PVD, /*IsUnknown=*/false);
      else if (capturesUnknown(PVD))
        SemaHelper->reportGlobalCapture(PVD, /*IsUnknown=*/true);
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
