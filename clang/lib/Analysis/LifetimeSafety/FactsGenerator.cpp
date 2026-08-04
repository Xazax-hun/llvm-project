//===- FactsGenerator.cpp - Lifetime Facts Generation -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <cassert>
#include <string>

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/StmtObjC.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/Analysis/Analyses/LifetimeSafety/FactsGenerator.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Origins.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/OperatorKinds.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TimeProfiler.h"

namespace clang::lifetimes::internal {
using llvm::isa_and_present;

OriginNode *FactsGenerator::getOriginNode(const ValueDecl &D) {
  return FactMgr.getOriginMgr().getOrCreateNode(&D);
}

OriginNode *FactsGenerator::getOriginNode(const Expr &E) {
  return FactMgr.getOriginMgr().getOrCreateNode(&E);
}

bool FactsGenerator::hasOrigins(QualType QT) const {
  return FactMgr.getOriginMgr().hasOrigins(QT);
}

bool FactsGenerator::hasOrigins(const Expr *E) const {
  return FactMgr.getOriginMgr().hasOrigins(E);
}

/// Propagates origin information from Src to Dst through all levels of
/// indirection, creating OriginFlowFacts at each level.
///
/// This function enforces a critical type-safety invariant: both trees
/// must have the same pointee-chain depth, and field children are
/// matched by `FieldDecl`. This invariant ensures that origins flow only
/// between compatible types during expression evaluation. Field pairs
/// found on both sides recurse; unmatched fields are skipped, which is
/// exercised by `CK_DerivedToBase` flows where Base's and Derived's
/// trees carry distinct direct-field FDs.
///
/// Examples:
///   - `int* p = &x;` flows origins from `&x` (depth 1) to `p` (depth 1)
///   - `int** pp = &p;` flows origins from `&p` (depth 2) to `pp` (depth 2)
///     * Level 1: pp <- p's address
///     * Level 2: (*pp) <- what p points to (i.e., &x)
///   - `View v = obj;` flows origins from `obj` (depth 1) to `v` (depth 1)
///   - `S s2 = s;` flows the top-level origin and recursively flows each
///     matching `FieldDecl` subtree, so loans on `s.v.inner` propagate to
///     `s2.v.inner`.
void FactsGenerator::flow(OriginNode *Dst, OriginNode *Src, bool Kill) {
  if (!Dst)
    return;
  assert(Src &&
         "Dst is non-null but Src is null. List must have the same length");
  assert(Dst->getLength() == Src->getLength() &&
         "Pointee chains must have the same length");

  while (Dst && Src) {
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        Dst->getOriginID(), Src->getOriginID(), Kill));
    for (const OriginNode::Edge &E : Dst->children())
      if (E.FD)
        if (OriginNode *SrcF = Src->getFieldChild(E.FD))
          flow(E.Child, SrcF, Kill);
    Dst = Dst->getPointeeChild();
    Src = Src->getPointeeChild();
  }
}

void FactsGenerator::flowSingleLevelWithUnknownDepth(OriginNode *Dst,
                                                     OriginNode *Src,
                                                     const Expr *LoanExpr,
                                                     bool Kill) {
  if (!Dst || !Src)
    return;
  // The intentional single-level flow: only the top-level (outer) origin.
  CurrentBlockFacts.push_back(
      FactMgr.createFact<OriginFlowFact>(Dst->getOriginID(),
                                         Src->getOriginID(), Kill));
  // Seed any deeper levels of the destination that the shallow flow did not
  // populate with an Unknown loan, so an inner borrow this flow could not carry
  // is reported as lost (the Unknown loan survives joins) rather than read from
  // a silently-empty origin a control-flow merge could mask.
  for (OriginNode *Inner = Dst->getPointeeChild(); Inner;
       Inner = Inner->getPointeeChild()) {
    const Loan *L = FactMgr.getLoanMgr().createLoan(
        AccessPath::Unknown(LoanExpr), LoanExpr);
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), Inner->getOriginID()));
  }
}

/// Creates a loan for the storage path of a given declaration reference.
/// This function should be called whenever a DeclRefExpr represents a borrow.
/// \param DRE The declaration reference expression that initiates the borrow.
/// \return The new Loan on success, nullptr otherwise.
static const Loan *createLoan(FactManager &FactMgr, const DeclRefExpr *DRE) {
  const ValueDecl *VD = DRE->getDecl();
  AccessPath Path(VD);
  // The loan is created at the location of the DeclRefExpr.
  return FactMgr.getLoanMgr().createLoan(Path, DRE);
}

/// Creates a loan for the storage location of a temporary object.
/// \param MTE The MaterializeTemporaryExpr that represents the temporary
/// binding. \return The new Loan.
static const Loan *createLoan(FactManager &FactMgr,
                              const MaterializeTemporaryExpr *MTE) {
  AccessPath Path(MTE);
  return FactMgr.getLoanMgr().createLoan(Path, MTE);
}

/// Creates a loan for an allocation through 'new'
/// \param NE The CXXNewExpr that represents the allocation
/// \return The new Loan on success, nullptr otherwise
static const Loan *createLoan(FactManager &FactMgr, const CXXNewExpr *NE) {
  AccessPath Path(NE);
  return FactMgr.getLoanMgr().createLoan(Path, NE);
}

/// `this->arr[i]` (and nested array subscripts, through parens/implicit casts)
/// denotes the array member `this->arr`: all elements share the array's single
/// origin, so a per-element access/store/mutation is one of the member itself.
/// Peels array subscripts to the underlying lvalue and returns it as a
/// MemberExpr (whose member type may be an array -- peel it with
/// arrayElementType), or null. Centralizes array-element normalization for the
/// AST-shape-based checks (const-subversion, self-referential store) so it is
/// done once rather than at every site.
static const MemberExpr *memberThroughArraySubscripts(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  while (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    const Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
    if (!Base->getType()->isArrayType())
      break; // a subscript of a pointer is an ordinary indirection, not this.
    E = Base;
  }
  return dyn_cast<MemberExpr>(E);
}

/// Peels array dimensions: `T[N]` / `T[N][M]` -> `T`. The element type a member
/// access through array subscripts effectively yields.
static QualType arrayElementType(QualType T) {
  while (const ArrayType *AT = T->getAsArrayTypeUnsafe())
    T = AT->getElementType();
  return T;
}

/// Returns true if a parameter of type `PT` lets the call mutate the owner the
/// argument refers to: a non-const pointer/reference to an owner (or to a record
/// that transitively contains a mutable owner field), or a gsl::Pointer that
/// exposes mutable access to a non-const owner pointee. Shared by the assumed-
/// invalidation and argument-overlap checks.
/// True if a non-const pointer/reference parameter of type `PT` may reach an
/// owner through its *dynamic* type even though its static pointee type reveals
/// none: the pointee is a polymorphic record, so a virtual call inside the callee
/// can dispatch back to a derived object that owns reallocatable storage. An
/// abstract interface is the extreme case -- it has no data members at all, yet
/// `void notify(Reloader &R) { R.reload(); }` can reallocate whatever the
/// most-derived object owns. Neither the static type nor any annotation expresses
/// that, so such a parameter is treated conservatively; the resulting
/// invalidation is loan-gated (OwnerLoanGate::DenotedOwner) so the checker acts
/// only when the argument actually denotes a mutable owner. Mirrors the
/// MaybeDynamicOwner case for a virtual-call receiver.
static bool paramMayReachDynamicOwner(QualType PT) {
  if (!PT->isPointerType() && !PT->isReferenceType())
    return false;
  QualType Pointee = PT->getPointeeType();
  if (Pointee.isConstQualified())
    return false;
  const CXXRecordDecl *RD = Pointee->getAsCXXRecordDecl();
  return RD && RD->hasDefinition() && RD->isPolymorphic();
}

static bool paramMayMutateOwner(QualType PT) {
  if (PT->isPointerType() || PT->isReferenceType()) {
    QualType Pointee = PT->getPointeeType();
    if (Pointee.isConstQualified())
      return false;
    if (isGslOwnerType(Pointee))
      return true;
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
    return recordContainsMutableOwner(Pointee->getAsCXXRecordDecl(), Visited);
  }
  if (isGslPointerType(PT.getNonReferenceType())) {
    if (pointsToMutableOwner(PT.getNonReferenceType()))
      return true;
    // A gsl::Pointer wrapper that reaches a mutable owner through a member it
    // aliases (e.g. a `std::vector<int>* v` member). A by-value copy still
    // aliases the same owner, so the callee can reallocate it through the copy,
    // invalidating borrows into it. (A by-value record that OWNS its owner is a
    // copy and must not be treated this way -- hence gated on gsl::Pointer.)
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
    return recordContainsMutableOwner(
        PT.getNonReferenceType()->getAsCXXRecordDecl(), Visited);
  }
  return false;
}

/// Maps a modeled call-argument index to the callee's corresponding
/// `ParmVarDecl`, or null when the argument has no declared parameter (the
/// implicit object argument of an implicit-`this` instance method, or a variadic
/// argument). The modeled argument list places the object first for instance
/// methods, so:
///   - implicit-`this` instance method: Args[0] is the object (no ParmVarDecl);
///     Args[I>=1] maps to getParamDecl(I - 1).
///   - C++23 explicit object member function: the object IS getParamDecl(0), so
///     Args[I] maps to getParamDecl(I) 1:1 (including the object at I == 0).
///   - free function / static method: Args[I] maps to getParamDecl(I).
/// `IsInstance` is the caller's "instance method with a leading object argument"
/// flag (false for constructors).
static const ParmVarDecl *paramForArg(const FunctionDecl *FD, bool IsInstance,
                                      unsigned I) {
  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  if (IsInstance && MD && !MD->isExplicitObjectMemberFunction()) {
    if (I == 0)
      return nullptr; // implicit object argument
    I -= 1;
  }
  return I < FD->getNumParams() ? FD->getParamDecl(I) : nullptr;
}

/// Recognizes a call into the setjmp/longjmp family, whose non-local control
/// flow the analysis cannot model (see UntrackedConstructReason::SetjmpLongjmp).
/// Matches the `returns_twice` attribute (which marks setjmp, and is present
/// even on a manual redeclaration), the setjmp/longjmp builtins, and a
/// C-linkage `longjmp`/`siglongjmp`/`_longjmp` (which carries no distinguishing
/// attribute or builtin id when redeclared by hand).
static bool isSetjmpLongjmp(const FunctionDecl *FD) {
  if (FD->hasAttr<ReturnsTwiceAttr>())
    return true;
  switch (FD->getBuiltinID()) {
  case Builtin::BI__builtin_setjmp:
  case Builtin::BI__builtin_longjmp:
  case Builtin::BI__sigsetjmp:
  case Builtin::BIsetjmp:
  case Builtin::BI_setjmp:
  case Builtin::BIlongjmp:
  case Builtin::BI_longjmp:
  case Builtin::BIsigsetjmp:
  case Builtin::BIsiglongjmp:
    return true;
  default:
    break;
  }
  if (FD->isExternC())
    if (const IdentifierInfo *II = FD->getIdentifier()) {
      StringRef N = II->getName();
      return N == "longjmp" || N == "siglongjmp" || N == "_longjmp";
    }
  return false;
}

/// Returns true if `MD` is a known non-invalidating accessor of a standard
/// library type -- one that returns a borrow into the container (or smart
/// pointer pointee) without reallocating it. Non-const member calls on an owner
/// are otherwise conservatively assumed to invalidate; this allow-list keeps the
/// common read accessors (`v[i]`, `v.at(i)`, `v.data()`, `m.find(k)`, `*p`, ...)
/// from being treated as mutating. Restricted to the std namespace: a user
/// type's accessors are not recognized and are treated conservatively.
static bool isNonInvalidatingMethod(const CXXMethodDecl &MD) {
  // The read-accessor allow-list encodes knowledge about *standard library*
  // owner types, so it must apply only to types in the genuine `std` namespace
  // -- including libc++'s inline versioning namespace `std::__1`, which has a
  // real `std` ancestor. The broader isInStlNamespace heuristic also treats any
  // top-level reserved-name namespace (`__detail`, `__gnu_cxx`, ...) as STL; a
  // user's custom [[gsl::Owner]] placed in such a namespace with a non-const,
  // reallocating method merely *named* like an accessor (`data`, `find`, ...)
  // must NOT be exempted -- it is conservatively assumed-invalidating.
  const DeclContext *DC = MD.getParent()->getDeclContext();
  bool InActualStd = false;
  for (; DC; DC = DC->getParent())
    if (DC->isStdNamespace()) {
      InActualStd = true;
      break;
    }
  if (!InActualStd)
    return false;
  switch (MD.getOverloadedOperator()) {
  case OO_Subscript: // operator[]
  case OO_Star:      // operator* (smart pointers, iterators)
  case OO_Arrow:     // operator->
    return true;
  default:
    break;
  }
  if (!MD.getIdentifier())
    return false;
  static const llvm::StringSet<> Accessors = {
      "at",     "data",   "c_str", "front",  "back",  "begin",
      "end",    "cbegin", "cend",  "rbegin", "rend",  "crbegin",
      "crend",  "find",   "get",   "top",    "lower_bound",
      "upper_bound", "equal_range"};
  return Accessors.contains(MD.getName());
}

void FactsGenerator::run() {
  llvm::TimeTraceScope TimeProfile("FactGenerator");
  const CFG &Cfg = *AC.getCFG();
  FlaggedIndirectionGlobals.clear();
  llvm::SmallVector<Fact *> PlaceholderLoanFacts = issuePlaceholderLoans();
  // Safe-model soundness: a coroutine's body is deferred past suspension points
  // and resumed later, possibly after a by-reference argument's temporary has
  // been destroyed. The analysis models the call as ordinary, so a borrowed
  // parameter used in the resumed body is never connected to that expiry. Reject
  // the construct (see UntrackedConstructReason::Coroutine). Emitted with the
  // entry-block placeholder facts (CurrentBlockFacts is cleared per block). Use
  // the declaration's raw body: AnalysisDeclContext::getBody() unwraps the
  // CoroutineBodyStmt to its inner statement.
  if (const auto *FD = dyn_cast_or_null<FunctionDecl>(AC.getDecl()))
    if (isa_and_present<CoroutineBodyStmt>(FD->getBody()))
      PlaceholderLoanFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::Coroutine, FD->getLocation()));
  // Iterate through the CFG blocks in reverse post-order to ensure that
  // initializations and destructions are processed in the correct sequence.
  for (const CFGBlock *Block : *AC.getAnalysis<PostOrderCFGView>()) {
    CurrentBlockFacts.clear();
    EscapesInCurrentBlock.clear();
    CurrentBlock = Block;
    if (Block == &Cfg.getEntry()) {
      CurrentBlockFacts.append(PlaceholderLoanFacts.begin(),
                               PlaceholderLoanFacts.end());
      handleTryStatements();
    }
    for (unsigned I = 0; I < Block->size(); ++I) {
      const CFGElement &Element = Block->Elements[I];
      if (std::optional<CFGStmt> CS = Element.getAs<CFGStmt>())
        Visit(CS->getStmt());
      else if (std::optional<CFGInitializer> Initializer =
                   Element.getAs<CFGInitializer>())
        handleCXXCtorInitializer(Initializer->getInitializer());
      else if (std::optional<CFGLifetimeEnds> LifetimeEnds =
                   Element.getAs<CFGLifetimeEnds>())
        handleLifetimeEnds(*LifetimeEnds);
      else if (std::optional<CFGCleanupFunction> CleanupFunction =
                   Element.getAs<CFGCleanupFunction>())
        handleCleanupFunction(*CleanupFunction);
      else if (std::optional<CFGFullExprCleanup> FullExprCleanup =
                   Element.getAs<CFGFullExprCleanup>()) {
        handleFullExprCleanup(*FullExprCleanup);
      }
    }
    if (Block == &Cfg.getExit())
      handleExitBlock();

    CurrentBlockFacts.append(EscapesInCurrentBlock.begin(),
                             EscapesInCurrentBlock.end());
    FactMgr.addBlockFacts(Block, CurrentBlockFacts);
  }
}

/// Simulates LValueToRValue conversion by peeling the outer lvalue origin
/// if the expression is a GLValue. For pointer/view GLValues, this strips
/// the origin representing the storage location to get the origins of the
/// pointed-to value.
///
/// Example: For `View& v`, returns the origin of what v points to, not v's
/// storage.
static OriginNode *getRValueOrigins(const Expr *E, OriginNode *Node) {
  if (!Node)
    return nullptr;
  return E->isGLValue() ? Node->getPointeeChild() : Node;
}

void FactsGenerator::VisitExpr(const Expr *E) {
  // Soundness catch-all. This is the ConstStmtVisitor fallback: it is reached
  // only by expression kinds with no specific Visit* handler. If such an
  // expression's TYPE carries a borrow (a pointer/reference/view) the generator
  // produced a value whose origin was never populated -- any borrow it should
  // carry is silently dropped (e.g. a C11 atomic builtin / AtomicExpr). Flag it
  // so the soundness model never silently fails on a construct it does not
  // model. Gate on the type, not hasOrigins(Expr) (which is true for every
  // glvalue), so a plain glvalue of non-borrow type is not flagged.
  if (!hasOrigins(E->getType()))
    return;
  // Skip kinds that are handled structurally elsewhere or are transparent
  // forwarders: their borrow flows through a sub-expression that is itself a
  // visited CFG element (or, for `this`, through the dedicated `this` origin).
  switch (E->getStmtClass()) {
  case Stmt::CXXThisExprClass:                  // modeled via the `this` origin
  case Stmt::ParenExprClass:                    // transparent
  case Stmt::ExprWithCleanupsClass:             // transparent (peeled)
  case Stmt::ConstantExprClass:                 // transparent
  case Stmt::SubstNonTypeTemplateParmExprClass: // transparent
  case Stmt::CXXDefaultArgExprClass:            // forwards to its sub-expr
  case Stmt::OpaqueValueExprClass:              // bound by its enclosing expr
    return;
  default:
    break;
  }
  CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
      UntrackedConstructReason::UnmodeledExpr, E));
}

void FactsGenerator::VisitDeclStmt(const DeclStmt *DS) {
  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (!VD)
      continue;
    // Soundness: a local of a user-defined type whose ownership is unknown.
    bool IsPointer = false;
    // An array of a borrow-holding element is just as untracked as the element;
    // peel array dimensions so `P a[N]` is flagged like the scalar `P a` (the
    // unknown-ownership / owner-/pointer-of-indirection checks otherwise bail on
    // an array type, whose getAsCXXRecordDecl() is null).
    QualType VDType = VD->getType();
    while (const ArrayType *AT = VDType->getAsArrayTypeUnsafe())
      VDType = AT->getElementType();
    if (isUnknownOwnershipType(VDType, FactMgr.getUnknownOwnershipCache()))
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::UnknownOwnership, VD));
    // Soundness: a local gsl::Owner container whose elements are indirections
    // (e.g. std::vector<int*>); per-element borrows are not tracked.
    else if (isGslOwnerOfIndirection(VDType,
                                     FactMgr.getUnknownOwnershipCache()))
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::OwnerOfIndirection, VD));
    // Likewise a local gsl::Pointer view whose pointee is an indirection (e.g.
    // std::span<int*>); the inner pointees are not tracked.
    else if (isGslPointerOfIndirection(VDType,
                                       FactMgr.getUnknownOwnershipCache()))
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::PointerOfIndirection, VD));
    // Or an owner-/pointer-of-indirection buried in the template arguments of a
    // non-owner aggregate (e.g. std::pair<std::vector<std::string_view>, int>).
    // Such an aggregate is neither a gsl::Owner nor gsl::Pointer, so the checks
    // above do not inspect its arguments; search them, mirroring the per-record
    // field-declaration check, so the local is rejected too. Report the precise
    // buried element/pointee type rather than the whole aggregate.
    else if (QualType Nested = findNestedOwnerOrPointerOfIndirection(
                 VDType, FactMgr.getUnknownOwnershipCache(), IsPointer);
             !Nested.isNull())
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          IsPointer ? UntrackedConstructReason::PointerOfIndirection
                    : UntrackedConstructReason::OwnerOfIndirection,
          VD, Nested));
    // An array of pointer-like elements shares one element-origin across all
    // elements. Seed it with a non-expiring "uninitialized" loan so the origin
    // is never empty: a borrow stored into an element later merges in beside
    // it, and an access before any store does not look like a lost borrow.
    if (VD->getType()->isArrayType() && hasOrigins(VD->getType()))
      if (OriginNode *VDNode = getOriginNode(*VD)) {
        const Loan *L = FactMgr.getLoanMgr().createLoan(
            AccessPath::Uninitialized(VD), /*IssuingExpr=*/nullptr);
        CurrentBlockFacts.push_back(
            FactMgr.createFact<IssueFact>(L->getID(), VDNode->getOriginID()));
      }
    if (const Expr *InitExpr = VD->getInit()) {
      if (OriginNode *VDNode = getOriginNode(*VD)) {
        OriginNode *InitNode = getOriginNode(*InitExpr);
        assert(InitNode && "VarDecl had origins but InitExpr did not");
        flow(VDNode, InitNode, /*Kill=*/true);
      }
    }
    // A structured binding's holding expressions (`e[i]` / `e.field`) are not
    // visited by the CFG walk; generate their flows so the bindings, which
    // alias them, are tracked. This must run even when the holding variable
    // itself has no origin list (e.g. a by-value `int[2]` copy): taking the
    // address of a binding still borrows the copy's storage.
    if (const auto *DD = dyn_cast<DecompositionDecl>(VD))
      handleStructuredBinding(DD);
  }
}

void FactsGenerator::handleStructuredBinding(const DecompositionDecl *DD) {
  for (const BindingDecl *BD : DD->bindings())
    if (const Expr *Holding = BD->getBinding())
      visitBindingHoldingExpr(Holding);
}

void FactsGenerator::visitBindingHoldingExpr(const Stmt *S) {
  if (!S)
    return;
  // Post-order: generate flows for sub-expressions (the holding variable
  // reference, array-to-pointer decay, etc.) before the access itself.
  for (const Stmt *Child : S->children())
    visitBindingHoldingExpr(Child);
  Visit(S);
}

void FactsGenerator::VisitDeclRefExpr(const DeclRefExpr *DRE) {
  // Skip function references as their lifetimes are not interesting. Skip non
  // GLValues (like EnumConstants).
  if (DRE->getFoundDecl()->isFunctionOrFunctionTemplate() || !DRE->isGLValue())
    return;
  // A read of a structured-binding element. A scalar element (`int a` in
  // `auto& [a, b] = obj;`) has no origin of its own, so handleUse below is a
  // no-op and the read does not register against the decomposed object's
  // borrow -- letting a use-after-free/scope of `obj` through the element slip
  // (e.g. `auto& [a, b] = *heapPtr; delete heapPtr; use(a);`). Mark the
  // decomposed object as used here so a borrow it holds stays live at this read
  // and an expiry/invalidation of the source is reported. (For a by-value
  // decomposition the decomposed object is an independent copy whose origin
  // holds no borrow into the source, so this adds no false positive.)
  if (const auto *BD = dyn_cast<BindingDecl>(DRE->getDecl()))
    if (const ValueDecl *Decomposed = BD->getDecomposedDecl())
      if (OriginNode *Node = getOriginNode(*Decomposed))
        CurrentBlockFacts.push_back(FactMgr.createFact<UseFact>(DRE, Node));
  handleUse(DRE);
  // Soundness: a use of a global "container of indirection" (owner/pointer whose
  // elements/pointees are themselves indirections) is banned by the model, just
  // like the local case in VisitDeclStmt. Flag it at the use site (the global's
  // declaration may be outside the analyzed region).
  if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
    if (VD->hasGlobalStorage())
      handleGlobalContainerOfIndirectionUse(DRE, VD);
  // For all declarations with storage (non-references), we issue a loan
  // representing the borrow of the variable's storage itself.
  //
  // Examples:
  //   - `int x; x` issues loan to x's storage
  //   - `int* p; p` issues loan to p's storage (the pointer variable)
  //   - `View v; v` issues loan to v's storage (the view object)
  //   - `int& r = x; r` issues no loan (r has no storage, it's an alias to x)
  if (doesDeclHaveStorage(DRE->getDecl())) {
    const Loan *L = createLoan(FactMgr, DRE);
    assert(L);
    OriginNode *Node = getOriginNode(*DRE);
    assert(Node &&
           "gl-value DRE of non-pointer type should have an origin list");
    // This loan specifically tracks borrowing the variable's storage location
    // itself and is issued to outermost origin (Node->OID).
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), Node->getOriginID()));
  }
}

void FactsGenerator::handleGlobalContainerOfIndirectionUse(
    const DeclRefExpr *DRE, const VarDecl *VD) {
  // Report each offending global at most once per function.
  if (!FlaggedIndirectionGlobals.insert(VD).second)
    return;
  // Peel array dimensions so a global array of such elements is treated like
  // the scalar (mirrors VisitDeclStmt).
  QualType T = VD->getType();
  while (const ArrayType *AT = T->getAsArrayTypeUnsafe())
    T = AT->getElementType();
  auto &Cache = FactMgr.getUnknownOwnershipCache();
  bool IsPointer = false;
  UntrackedConstructReason Reason;
  QualType ReportType;
  // A gsl::Owner whose elements are indirections (e.g. std::vector<int*>,
  // std::vector<std::string_view>); per-element borrows are not tracked.
  if (isGslOwnerOfIndirection(T, Cache))
    Reason = UntrackedConstructReason::OwnerOfIndirection;
  // A gsl::Pointer view whose pointee is an indirection (e.g. std::span<int*>).
  else if (isGslPointerOfIndirection(T, Cache))
    Reason = UntrackedConstructReason::PointerOfIndirection;
  // Or such a type buried in a non-owner aggregate (e.g.
  // std::pair<std::vector<std::string_view>, int>).
  else if (QualType Nested =
               findNestedOwnerOrPointerOfIndirection(T, Cache, IsPointer);
           !Nested.isNull()) {
    Reason = IsPointer ? UntrackedConstructReason::PointerOfIndirection
                       : UntrackedConstructReason::OwnerOfIndirection;
    ReportType = Nested;
  } else
    return; // Not a container of indirection; nothing to flag.
  CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
      Reason, cast<Expr>(DRE), ReportType));
}

void FactsGenerator::VisitCXXConstructExpr(const CXXConstructExpr *CCE) {
  if (isGslPointerType(CCE->getType())) {
    handleGSLPointerConstruction(CCE);
    return;
  }
  // For defaulted (implicit or `= default`) copy/move constructors, propagate
  // origins directly. User-defined copy/move constructors are not handled here
  // as they have opaque semantics.
  if (CCE->getConstructor()->isCopyOrMoveConstructor() &&
      CCE->getConstructor()->isDefaulted() && CCE->getNumArgs() == 1 &&
      hasOrigins(CCE->getType())) {
    const Expr *Arg = CCE->getArg(0);
    if (OriginNode *ArgNode = getRValueOrigins(Arg, getOriginNode(*Arg))) {
      flow(getOriginNode(*CCE), ArgNode, /*Kill=*/true);
      return;
    }
  }
  // Standard library callable wrappers (e.g., std::function) propagate the
  // stored lambda's origins.
  if (const auto *RD = CCE->getType()->getAsCXXRecordDecl();
      RD && isStdCallableWrapperType(RD) && CCE->getNumArgs() == 1) {
    const Expr *Arg = CCE->getArg(0);
    if (OriginNode *ArgNode = getRValueOrigins(Arg, getOriginNode(*Arg))) {
      flow(getOriginNode(*CCE), ArgNode, /*Kill=*/true);
      return;
    }
  }
  handleFunctionCall(CCE, CCE->getConstructor(),
                     {CCE->getArgs(), CCE->getNumArgs()},
                     /*IsGslConstruction=*/false);
  // Soundness: a constructor temporary of a borrow-holding non-gsl record
  // (e.g. `Box(&local)` where `struct Box { int* p; }` with a capturing ctor)
  // is, like an escaping aggregate temporary, untracked -- the borrow it
  // captures is dropped (capture_by on a constructor is unmodeled) and the
  // record's ownership is unknown. Reported at a local/member declaration and a
  // call result, but a bare constructor temporary that is member-accessed /
  // returned / stored is covered by neither, so flag it here (the
  // declaration-initializer form is skipped via the parent walk). A copy/move
  // construction is excluded -- it does not create a new borrow-holder, it
  // copies an existing (already-reported) one, so flagging it would
  // double-report (e.g. on `return p;`).
  if (!CCE->getConstructor()->isCopyOrMoveConstructor())
    maybeReportUntrackedAggregateTemporary(CCE);
}

void FactsGenerator::VisitCXXDefaultInitExpr(const CXXDefaultInitExpr *DIE) {
  if (const Expr *Init = DIE->getExpr())
    killAndFlowOrigin(*DIE, *Init);
}

void FactsGenerator::handleCXXCtorInitializer(const CXXCtorInitializer *CII) {
  // Flows origins from the initializer expression to the field.
  // Example: `MyObj(std::string s) : view(s) {}`
  const FieldDecl *FD = CII->getAnyMember();
  if (!FD) {
    // Soundness: a base-class initializer (`: Base(init)`) stores into the base
    // subobject of `this`. If that subobject is (or holds) an indirection -- for
    // instance a [[gsl::Owner]] that privately inherits std::string_view -- the
    // borrow in `init` is captured into the object exactly as a store into a
    // data member would be. But the origin model does not represent base
    // subobjects (hasOrigins inspects only public data members, not bases), so
    // this store was invisible: a lying [[clang::noescape]] initializer argument
    // escaped into the object undetected. Model it as a capture into `this` --
    // mirroring lifetime_capture_by(this) -- so the annotation verifier sees the
    // escaped parameter loan. The escaped loans are inspected by checkAnnotations
    // and only turn into a diagnostic for a noescape argument; a lifetimebound or
    // unannotated one is handled by its own (unchanged) path.
    if (CII->isBaseInitializer()) {
      QualType BaseTy(CII->getBaseClass(), 0);
      const Expr *Init = CII->getInit();
      if (Init && FactMgr.getOriginMgr().hasOrigins(BaseTy) &&
          FactMgr.getOriginMgr().hasOrigins(Init) &&
          FactMgr.getOriginMgr().getThisOrigins())
        if (OriginNode *InitNode = getOriginNode(*Init)) {
          InitNode = getRValueOrigins(Init, InitNode);
          CurrentBlockFacts.push_back(
              FactMgr.createFact<CapturedByThisEscapeFact>(
                  InitNode->getOriginID(), Init));
        }
    }
    return;
  }
  killAndFlowOrigin(*FD, *CII->getInit());
  // Soundness: a member-initializer is a store into the member, just like
  // `this->view = init;` in the constructor body. Record a FieldStore for a
  // view/pointer member so the checker can detect a self-referential object --
  // one whose member borrows a sibling member of the same object (e.g.
  // `S() : view(buf) {}`). Without this only the body-store spelling was caught.
  // The detection is loan-based (the checker intersects the stored value's loans
  // with the enclosing `this` object's), so it is independent of the init shape.
  if (isGslPointerType(arrayElementType(FD->getType())) ||
      arrayElementType(FD->getType())->isPointerOrReferenceType())
    if (std::optional<OriginNode *> ThisOrigins =
            FactMgr.getOriginMgr().getThisOrigins())
      if (OriginNode *InitNode = getOriginNode(*CII->getInit()))
        CurrentBlockFacts.push_back(FactMgr.createFact<FieldStoreFact>(
            CII->getInit(), InitNode->getOriginID(),
            (*ThisOrigins)->getOriginID()));
}

void FactsGenerator::VisitCXXMemberCallExpr(const CXXMemberCallExpr *MCE) {
  // Specifically for conversion operators,
  // like `std::string_view p = std::string{};`
  if (isGslPointerType(MCE->getType()) &&
      isa_and_present<CXXConversionDecl>(MCE->getCalleeDecl()) &&
      isGslOwnerType(MCE->getImplicitObjectArgument()->getType())) {
    // The argument is the implicit object itself.
    handleFunctionCall(MCE, MCE->getMethodDecl(),
                       {MCE->getImplicitObjectArgument()},
                       /*IsGslConstruction=*/true);
    return;
  }
  if (const CXXMethodDecl *Method = MCE->getMethodDecl()) {
    // Construct the argument list, with the implicit 'this' object as the
    // first argument.
    llvm::SmallVector<const Expr *, 4> Args;
    Args.push_back(MCE->getImplicitObjectArgument());
    Args.append(MCE->getArgs(), MCE->getArgs() + MCE->getNumArgs());

    handleFunctionCall(MCE, Method, Args, /*IsGslConstruction=*/false);
  } else {
    // No resolved method (e.g. a call through a member-function pointer): the
    // callee cannot carry lifetime annotations, so the call is not modeled.
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::IndirectCall, MCE));
  }
}

void FactsGenerator::VisitMemberExpr(const MemberExpr *ME) {
  auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
  if (!FD)
    return;

  // Safe-model soundness: a union member access is not modeled. Different union
  // members alias the same storage, so a borrow into one member can be
  // invalidated by writing another (or by switching the active member);
  // invalidation is keyed by field identity, which does not see this aliasing,
  // so the borrow can dangle undetected. Surface the access as unsupported.
  if (FD->getParent()->isUnion())
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::Union, cast<Expr>(ME)));

  assert(ME->isGLValue() && "Field member should be GL value");
  OriginNode *Dst = getOriginNode(*ME);
  assert(Dst && "Field member should have an origin list as it is GL value");

  OriginNode *Src = getOriginNode(*ME->getBase());
  if (doesDeclHaveStorage(FD)) {
    assert(Src && "Base expression should be a pointer/reference type");

    // Safe-model soundness: a borrow of an OWNER field (e.g. `this->buf` where
    // `buf` is a std::string/std::vector) borrows the field's heap buffer.
    // Issue a field-rooted loan -- mirroring the storage loan a *local* owner
    // gets in VisitDeclRefExpr -- so a later mutation of the field, whether
    // directly (`buf.append(...)`) or via a non-const method on the containing
    // object, can invalidate views into it.
    //
    // The MemberExpr origin must end up holding BOTH this field loan and the
    // base's loans (notably `$this`, which lifetimebound-`this` verification of
    // borrow-returning accessors relies on). Since IssueFact *replaces* an
    // origin's loan set, issue the field loan first (the MemberExpr origin is
    // fresh) and then *merge* the base flow (Kill=false) rather than replacing.
    // Issue the field-rooted owner loan only when the field access denotes
    // STABLE storage -- i.e. the access is an lvalue (`this->buf`, a local
    // `h.buf`). When the base is a materialized temporary, the field access is an
    // xvalue (`Holder{}.s`): the owner subobject lives only as long as the
    // temporary (a lifetime-extended-temporary reference does not give it
    // borrowable storage, Origins.cpp TODO), so manufacturing a FieldDecl-rooted
    // loan here would launder a lost loan into a tracked one that never expires.
    // Leave such a borrow lost so it surfaces as -Wlifetime-safety-lost-loan,
    // matching the direct `const std::string& r = std::string(...)` form.
    bool OwnerField = isMutableOwnerType(FD->getType()) && ME->isLValue();
    if (OwnerField) {
      const Loan *L = FactMgr.getLoanMgr().createLoan(AccessPath(FD), ME);
      CurrentBlockFacts.push_back(
          FactMgr.createFact<IssueFact>(L->getID(), Dst->getOriginID()));
    }
    // The field's glvalue (outermost origin) holds the same loans as the base
    // expression.
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        Dst->getOriginID(), Src->getOriginID(),
        /*Kill=*/!OwnerField));
  }

  // Only narrow when the field is in the base's tree; otherwise the
  // MemberExpr resolved to an orphan disconnected from the base (e.g., a
  // `[[gsl::Pointer]]` base whose fields aren't expanded), and narrowing
  // would drop the base's root liveness, so a loan deposited on the root
  // via `lifetime_capture_by(this)` would be missed.
  if (Src && Src->getFieldChildInChain(FD)) {
    // Narrow the UseFact's liveness coverage to the accessed field's
    // subtree.
    //
    // E.g., for `(void)s.inner`, without narrowing, the UseFact at `s`
    // would keep `s.v`'s subtree live and falsely flag a UAF when a loan
    // held by `s.v` has already expired.
    if (UseFact *UF = UseFacts.lookup(ME->getBase())) {
      assert(!UseFacts.contains(ME) && "ME already has a UseFact");
      OriginNode *NewUsedOrigins =
          doesDeclHaveStorage(FD) ? Dst->getPointeeChild() : Dst;
      UF->setUsedOrigins(NewUsedOrigins);
      UseFacts[ME] = UF;
      UseFacts.erase(ME->getBase());
    }
  }

  // Safe-model soundness (read-side dual of the view-member store merge in
  // handleAssignment): when the base is a [[gsl::Pointer]] leaf -- whose fields
  // are not expanded, so the field is not in the base's origin tree -- and the
  // field is itself borrow-holding, a READ of the field yields the borrow the
  // view holds. The view carries that borrow on its own value origin (the
  // rvalue of the base); flow it into the member-read's value so `out = v.p`
  // extracts the held borrow instead of dropping it into the disconnected
  // member-access origin. (A store into v.p symmetrically merges into the view's
  // origin; without this a later read would not see the borrow, and a control-
  // flow merge supplying a valid loan on another path could mask the lost-loan
  // backstop.)
  if (Src && !Src->getFieldChildInChain(FD) &&
      isGslPointerType(ME->getBase()->getType().getNonReferenceType()) &&
      hasOrigins(FD->getType())) {
    OriginNode *MemberVal =
        doesDeclHaveStorage(FD) ? Dst->getPointeeChild() : Dst;
    OriginNode *ViewBorrow = getRValueOrigins(ME->getBase(), Src);
    if (MemberVal && ViewBorrow &&
        MemberVal->getLength() == ViewBorrow->getLength())
      flow(MemberVal, ViewBorrow, /*Kill=*/false);
  }
}

void FactsGenerator::VisitCallExpr(const CallExpr *CE) {
  // Safe-model soundness: a setjmp/longjmp family call introduces non-local
  // control flow. `longjmp` transfers back to a `setjmp` point, which the CFG
  // does not model as a back-edge, so a borrow invalidated on the first pass and
  // read after the jump re-enters can dangle undetected. Reject the construct.
  if (const FunctionDecl *Callee = CE->getDirectCallee())
    if (isSetjmpLongjmp(Callee))
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::SetjmpLongjmp, cast<Expr>(CE)));
  handleFunctionCall(CE, CE->getDirectCallee(),
                     {CE->getArgs(), CE->getNumArgs()});
}

void FactsGenerator::VisitCXXNullPtrLiteralExpr(
    const CXXNullPtrLiteralExpr *N) {
  /// TODO: Handle nullptr expr as a special 'null' loan. Uninitialized
  /// pointers can use the same type of loan.
  getOriginNode(*N);
}

void FactsGenerator::VisitCastExpr(const CastExpr *CE) {
  // Safe-model soundness: a reinterpret_cast can launder a borrow through an
  // unrelated type (reinterpreting storage), hiding its provenance, so a borrow
  // recovered through it is not tracked. Surface it as unsupported, regardless
  // of the cast kind or whether the result has a trackable origin.
  if (isa<CXXReinterpretCastExpr>(CE))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::ReinterpretCast, cast<Expr>(CE)));

  OriginNode *Dest = getOriginNode(*CE);
  if (!Dest)
    return;
  const Expr *SubExpr = CE->getSubExpr();
  OriginNode *Src = getOriginNode(*SubExpr);

  switch (CE->getCastKind()) {
  case CK_LValueToRValue:
    if (!SubExpr->isGLValue())
      return;

    assert(Src && "LValue being cast to RValue has no origin list");
    // The result of an LValue-to-RValue cast on a pointer lvalue (like `q` in
    // `int *p, *q; p = q;`) should propagate the inner origin (what the pointer
    // points to), not the outer origin (the pointer's storage location). Strip
    // the outer lvalue origin.
    flow(getOriginNode(*CE), getRValueOrigins(SubExpr, Src),
         /*Kill=*/true);
    return;
  case CK_NullToPointer:
    getOriginNode(*CE);
    // TODO: Flow into them a null origin.
    return;
  case CK_NoOp:
  case CK_ConstructorConversion:
  case CK_UserDefinedConversion:
    flow(Dest, Src, /*Kill=*/true);
    return;
  case CK_UncheckedDerivedToBase:
  case CK_DerivedToBase:
    // It is possible that the derived class and base class have different
    // gsl::Pointer annotations. Skip if their origin shape differ.
    if (Dest && Src && Dest->getLength() == Src->getLength())
      flow(Dest, Src, /*Kill=*/true);
    return;
  case CK_BaseToDerived:
  case CK_Dynamic:
    // A downcast (`static_cast<Derived*>(base)`, `dynamic_cast<Derived&>(base)`)
    // denotes the same object as its operand, so the object's loan must carry
    // through -- otherwise a borrow taken via `static_cast<Derived*>(b)->member`
    // roots at nothing and a later `delete b` / scope exit is never connected
    // (a silent use-after-free). If the derived/base origin shapes line up, flow
    // precisely; otherwise (derived adds owner fields / differing gsl::Pointer
    // annotations, so the loan trees cannot be mapped) carry the outer object
    // loan and seed the deeper levels with an Unknown loan, so any inner borrow
    // this flow could not carry surfaces as -Wlifetime-safety-lost-loan rather
    // than being silently dropped.
    if (Dest && Src) {
      if (Dest->getLength() == Src->getLength())
        flow(Dest, Src, /*Kill=*/true);
      else
        flowSingleLevelWithUnknownDepth(Dest, Src, CE, /*Kill=*/true);
    }
    return;
  case CK_ArrayToPointerDecay:
    assert(Src && "Array expression should have origins as it is GL value");
    // Soundness: an array whose element type is itself an indirection (a
    // pointer, reference, or view) decays to a pointer-to-pointer -- a double
    // level of indirection the analysis cannot model (mirrors the `int**` / `&p`
    // rule, and std::vector<int*> being rejected; per-element borrows cannot be
    // tracked). Reject the decay EXCEPT as the immediate base of an `arr[i]`
    // subscript, which is a single-level element access that IS modeled. So
    // `arr[i]` stays usable, while escaping the decay as a bare
    // pointer-to-pointer (`*(c ? arr : arr2)`, `*(arr + i)`, passing `arr` to an
    // `int**` parameter) is flagged. (`int arr[]` / `char buf[]` decay is a
    // single level and fine.)
    if (const auto *AT = CE->getSubExpr()->getType()->getAsArrayTypeUnsafe();
        AT && (isPointerLikeType(AT->getElementType()) ||
               AT->getElementType()->isReferenceType())) {
      const auto *ASE =
          dyn_cast_or_null<ArraySubscriptExpr>(AC.getParentMap().getParent(CE));
      if (!ASE || ASE->getBase() != CE)
        CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
            UntrackedConstructReason::ArrayOfIndirectionDecay, cast<Expr>(CE)));
    }
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        Dest->getOriginID(), Src->getOriginID(), /*Kill=*/true));
    return;
  case CK_FunctionToPointerDecay:
  case CK_BuiltinFnToFnPtr:
    // Ignore function-to-pointer decays.
    return;
  case CK_BitCast:
    // OriginLists for Src and Dst may differ here. For example when casting
    // from int** to void*
    if (Src && Dest && Dest->getLength() == Src->getLength())
      flow(Dest, Src, /*Kill=*/true);
    return;
  case CK_LValueToRValueBitCast: {
    // `__builtin_bit_cast` / `std::bit_cast` reinterprets the object
    // representation. A pointer-to-pointer bit-cast preserves the value, so
    // propagate the borrow (a `return __builtin_bit_cast(T*, &local)` is then
    // caught as return-stack-address, matching reinterpret_cast). A bit-cast that
    // materializes a pointer from a non-pointer representation (an integer
    // round-trip) launders the borrow's provenance and yields no matching source
    // origin -- mark a use so the result surfaces as a lost loan (mirrors
    // CK_IntegralToPointer). The operand may be a materialized glvalue, so strip
    // its outer lvalue level first.
    OriginNode *RVSrc = getRValueOrigins(SubExpr, Src);
    if (RVSrc && Dest->getLength() == RVSrc->getLength())
      flow(Dest, RVSrc, /*Kill=*/true);
    else
      handleUse(CE);
    return;
  }
  case CK_NonAtomicToAtomic:
  case CK_AtomicToNonAtomic: {
    // Wrapping/unwrapping an atomic preserves the underlying pointer value, so
    // propagate the origin. Without this a borrow stored into / read from an
    // `_Atomic(T*)` / `std::atomic<T*>` was silently dropped (e.g. on a direct
    // `return` of the atomic). The operand may be an lvalue, so strip its outer
    // lvalue level first.
    OriginNode *RVSrc = getRValueOrigins(SubExpr, Src);
    if (RVSrc && Dest->getLength() == RVSrc->getLength())
      flow(Dest, RVSrc, /*Kill=*/true);
    return;
  }
  case CK_IntegralToPointer:
    // A pointer materialized from an integer (e.g. 'reinterpret_cast<int*>(n)'
    // or a C-style '(int*)n') has no tracked provenance: any borrow laundered
    // through the integer was dropped. Mark the result as a use so the empty
    // origin is reported as a lost loan -- this covers the result being
    // dereferenced/indexed directly as an rvalue, which would otherwise create
    // no tracked pointer lvalue and slip through.
    handleUse(CE);
    return;
  default:
    // An unhandled cast kind whose result carries origins: rather than silently
    // dropping whatever borrow the operand held (which can hide a use-after-free
    // -- e.g. the base<->derived cast family above), carry the operand's outer
    // loan and seed any deeper levels with an Unknown loan, so a dropped inner
    // borrow surfaces as -Wlifetime-safety-lost-loan. The outer level flows
    // precisely, so a genuinely borrow-free result stays empty (no false
    // positive); only an unmodeled deeper indirection is flagged.
    if (Dest && Src)
      flowSingleLevelWithUnknownDepth(Dest, Src, CE, /*Kill=*/true);
    return;
  }
}

// Defined below; declared here for use in VisitUnaryOperator.

void FactsGenerator::VisitUnaryOperator(const UnaryOperator *UO) {
  switch (UO->getOpcode()) {
  case UO_AddrOf: {
    const Expr *SubExpr = UO->getSubExpr();
    // The origin of an address-of expression (e.g., &x) is the origin of
    // its sub-expression (x). This fact will cause the dataflow analysis
    // to propagate any loans held by the sub-expression's origin to the
    // origin of this UnaryOperator expression.
    killAndFlowOrigin(*UO, *SubExpr);
    // Soundness: taking the address of an indirection (`&p` where `p` is a
    // pointer or a view) forms a second level of indirection that the analysis
    // cannot fully model -- the same single-indirection rule the model enforces
    // on declarations, applied to transient expressions (which can otherwise
    // build a double indirection no declaration captures, e.g. `*&sv = q` or
    // `*(c ? &a : &b) = q`). Taking the address of a non-indirection (an owner,
    // a scalar) stays a single level and is fine.
    if (isPointerLikeType(SubExpr->getType().getNonReferenceType()))
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::MultiLevelIndirectionExpr, UO));
    return;
  }
  case UO_Deref: {
    const Expr *SubExpr = UO->getSubExpr();
    killAndFlowOrigin(*UO, *SubExpr);
    return;
  }
  case UO_Plus:
  case UO_PreInc:
  case UO_PostInc:
  case UO_PreDec:
  case UO_PostDec: {
    // Incrementing/decrementing a pointer keeps it aimed into the same
    // allocation, and unary plus on a pointer is the identity -- so the result
    // carries the operand's loans. Without this a borrow used via the result
    // (e.g. `g = ++p`, or `p = +&local`) is dropped, leaving an *empty* origin
    // that a control-flow merge supplying a valid loan on another path would
    // mask (suppressing the lost-loan backstop). Pre-inc/dec yields the operand
    // (an lvalue, same origin shape); post-inc/dec and unary plus yield a
    // prvalue (one level shallower) -- match the value category. Non-pointer
    // operands have no origins to flow.
    if (!UO->getType()->isPointerType())
      return;
    const Expr *SubExpr = UO->getSubExpr();
    OriginNode *SubNode = getOriginNode(*SubExpr);
    OriginNode *Src =
        UO->isGLValue() ? SubNode : getRValueOrigins(SubExpr, SubNode);
    if (Src)
      flow(getOriginNode(*UO), Src, /*Kill=*/true);
    return;
  }
  case UO_Real:
  case UO_Imag: {
    // `__real__ obj` / `__imag__ obj` name a component of `obj` (a glvalue when
    // the operand is a glvalue), so `&__real__ obj` borrows obj's storage.
    // Propagate the operand's origin so the borrow is tracked; without this the
    // address-of yields an empty origin and the borrow is dropped.
    killAndFlowOrigin(*UO, *UO->getSubExpr());
    return;
  }
  default:
    // Any other unary operator that yields a borrow-carrying value the analysis
    // does not model precisely must not leave its result origin *empty*: an
    // empty origin is masked by a control-flow merge supplying a valid loan on
    // another path, suppressing the lost-loan backstop (unlike the join-immune
    // Unknown sentinel). Seed an Unknown loan so a dropped borrow surfaces as
    // lost-loan robustly across joins. (Value-preserving forms above propagate
    // the operand's loans precisely; this covers anything not handled there.)
    issueUnknownLoanIfUntrackedBorrow(UO, getOriginNode(*UO),
                                      /*FlowedIntoResult=*/false);
    return;
  }
}

void FactsGenerator::emitReturnEscapes(OriginNode *N, const Expr *RetExpr) {
  if (!N)
    return;
  EscapesInCurrentBlock.push_back(
      FactMgr.createFact<ReturnEscapeFact>(N->getOriginID(), RetExpr));
  for (const OriginNode::Edge &E : N->children())
    emitReturnEscapes(E.Child, RetExpr);
}

void FactsGenerator::VisitReturnStmt(const ReturnStmt *RS) {
  if (const Expr *RetExpr = RS->getRetValue())
    emitReturnEscapes(getOriginNode(*RetExpr), RetExpr);
}

void FactsGenerator::handleAssignment(const Expr *TargetExpr,
                                      const Expr *LHSExpr,
                                      const Expr *RHSExpr) {
  LHSExpr = LHSExpr->IgnoreParenImpCasts();
  // Look through a value-preserving explicit reference cast on the destination
  // (e.g. `static_cast<int*&>(p) = ...` or the C-style `(int*&)p = ...`), which
  // preserves the underlying lvalue but is not stripped by IgnoreParenImpCasts.
  // Without this the store would route to no origin and be silently dropped. A
  // reinterpret_cast is not CK_NoOp (it stays flagged as type punning) and a
  // const_cast keeps its const-subversion diagnostic, both emitted independently
  // when the cast expression is visited.
  while (const auto *ECE = dyn_cast<ExplicitCastExpr>(LHSExpr)) {
    if (ECE->getCastKind() != CK_NoOp)
      break;
    LHSExpr = ECE->getSubExpr()->IgnoreParenImpCasts();
  }

  OriginNode *LHSNode = nullptr;
  QualType LHSType;
  UseFact *LHSUseFact = nullptr;

  if (const auto *DRE_LHS = dyn_cast<DeclRefExpr>(LHSExpr)) {
    LHSNode = getOriginNode(*DRE_LHS);
    assert(LHSNode && "LHS is a DRE and should have an origin list");
    LHSType = DRE_LHS->getDecl()->getType();
    LHSUseFact = UseFacts.lookup(DRE_LHS);
  } else if (const auto *ME_LHS = dyn_cast<MemberExpr>(LHSExpr)) {
    // Handle assignment to member fields (e.g., `this->view = s` or `view =
    // s`). This enables detection of dangling fields when local values escape
    // to fields.
    LHSNode = getOriginNode(*ME_LHS);
    assert(LHSNode && "LHS is a MemberExpr and should have an origin list");
    LHSType = ME_LHS->getMemberDecl()->getType();
    LHSUseFact = UseFacts.lookup(ME_LHS);
  }
  // Assignment to an array element (`arr[i] = &x`, or equivalently
  // `*(arr+i) = &x`). All elements share the array's single element-origin, so
  // we cannot tell which element is overwritten: merge the new loans in rather
  // than killing the old ones (the origin conservatively holds the loans of
  // every element ever stored). Route the store to the array OBJECT's origin so
  // both the subscript and the decayed-pointer-dereference store forms land on
  // the same shared element-origin that element reads observe.
  bool MergeIntoSharedElement = false;
  if (const Expr *ArrObj = getArrayObjectOfElementAccess(LHSExpr)) {
    LHSNode = getOriginNode(*ArrObj);
    // Soundness: for the genuine subscript form `base[i] = ...`, the array
    // object must be stable storage that a later element read re-resolves to --
    // a declared array or a `this`/field array. A selecting/forwarding base
    // (`(c ? a : b)[i] = ...`, `(f(), a)[i] = ...`) yields a transient
    // element-origin produced by a one-way merge, which a store cannot be routed
    // back through to the real arrays -- the same limitation as the scalar
    // selecting-lvalue case. Leaving LHSNode null lets the unroutable-store
    // catch-all below reject it (-Wlifetime-safety-unsupported-store) rather than
    // silently dropping the borrow into the transient origin (where the
    // uninitialized-array sentinel would also mask the lost loan). The `*(...)`
    // deref form is already rejected by the array-of-indirection-decay rule, so
    // restrict this to ArraySubscriptExpr to avoid double-flagging it.
    if (isa<ArraySubscriptExpr>(LHSExpr) && LHSNode &&
        !FactMgr.getOriginMgr().isStableStorageOrigin(LHSNode))
      LHSNode = nullptr;
    else
      MergeIntoSharedElement = LHSNode != nullptr;
  }

  if (!LHSNode) {
    // The destination could not be routed to a tracked storage origin. This
    // happens for an lvalue that selects/forwards among several objects -- a
    // conditional `(c ? p : q) = ...`, a comma `(f(), p) = ...`, or those
    // wrapped in `*&(...)`/casts -- whose origin is a transient merge that a
    // store cannot propagate back to the real objects (the same one-way-merge
    // limitation as the array-of-pointers case). Rather than enumerate every
    // such spelling, conservatively flag any unroutable store whose destination
    // TYPE holds a borrow (a pointer/view): a borrow stored here is dropped, and
    // (masked by a pre-existing concrete loan on the real object) a later
    // dangling use could be missed. Gate on the type, not hasOrigins(Expr) --
    // the latter is true for every glvalue, so it would flag a plain
    // `cells[i] = ' '` char store through operator[].
    if (hasOrigins(LHSExpr->getType()))
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::UnsupportedStoreDestination,
          cast<Expr>(LHSExpr)));
    return;
  }
  OriginNode *RHSNode = getOriginNode(*RHSExpr);
  // For operator= with reference parameters (e.g.,
  // `View& operator=(const View&)`), the RHS argument stays an lvalue,
  // unlike built-in assignment where LValueToRValue cast strips the outer
  // lvalue origin. Strip it manually to get the actual value origins being
  // assigned.
  RHSNode = getRValueOrigins(RHSExpr, RHSNode);

  if (LHSUseFact) {
    if (LHSType->isReferenceType()) {
      if (hasOrigins(LHSType->getPointeeType())) {
        // Writing through a reference uses the binding but overwrites the
        // pointee. Model this as a Read of the outer origin (keeping the
        // binding live) and a Write of the inner origins (killing the pointee's
        // liveness).
        const OriginNode *FullNode = LHSUseFact->getUsedOrigins();
        assert(FullNode);
        LHSUseFact->setUsedOrigins(
            FactMgr.getOriginMgr().createSingleOriginNode(
                FullNode->getOriginID()));
        if (const OriginNode *InnerNode = FullNode->getPointeeChild()) {
          UseFact *WriteUF = FactMgr.createFact<UseFact>(LHSExpr, InnerNode);
          WriteUF->markAsWritten();
          CurrentBlockFacts.push_back(WriteUF);
        }
      }
    } else
      LHSUseFact->markAsWritten();
  }
  if (!RHSNode) {
    // RHS has no tracked origins (e.g., assigning a callable without origins
    // to std::function). Clear loans of the destination.
    for (OriginNode *LHSInner = LHSNode->getPointeeChild(); LHSInner;
         LHSInner = LHSInner->getPointeeChild())
      CurrentBlockFacts.push_back(
          FactMgr.createFact<KillOriginFact>(LHSInner->getOriginID()));
    return;
  }
  // Kill the old loans of the destination origin and flow the new loans
  // from the source origin. For a shared array element-origin we merge instead
  // of killing (see above).
  flow(LHSNode->getPointeeChild(), RHSNode, /*Kill=*/!MergeIntoSharedElement);
  killAndFlowOrigin(*TargetExpr, *LHSExpr);

  // Soundness: record a store into a view/pointer member so the checker can
  // detect a self-referential object -- one where the stored value borrows the
  // same object that holds the member (e.g. `this->view = this->str;`). The
  // detection is loan-based (the checker intersects the stored value's loans
  // with the enclosing object's), so it sees borrows laundered through function
  // calls and is independent of the assignment's syntactic shape.
  if (const auto *ME_LHS = memberThroughArraySubscripts(LHSExpr))
    if (const auto *LF = dyn_cast<FieldDecl>(ME_LHS->getMemberDecl());
        LF && (isGslPointerType(arrayElementType(LF->getType())) ||
               arrayElementType(LF->getType())->isPointerOrReferenceType())) {
      // Use the static type of the *receiver object* as the enclosing object,
      // not the type that declares the member. When the member lives in a base
      // class, `ME_LHS->getBase()` is the receiver implicitly cast to that base
      // (`(Base*)this`); stripping the implicit derived-to-base cast recovers
      // the most-derived receiver (`this` of type `Derived*`), so a view in a
      // base subobject bound to a member of the derived class is recognized as
      // self-referential too -- the membership walk then sees the derived
      // class's fields.
      const Expr *Base = ME_LHS->getBase()->IgnoreImpCasts();
      if (OriginNode *Container = getOriginNode(*Base)) {
        CurrentBlockFacts.push_back(FactMgr.createFact<FieldStoreFact>(
            ME_LHS, RHSNode->getOriginID(), Container->getOriginID()));
        // See through anonymous struct/union members: for `v.p` where `p` lives
        // in an anonymous struct inside a [[gsl::Pointer]] `V`, the assigned
        // member's base is the unnamed anonymous-record subobject (whose type is
        // not a gsl::Pointer), so testing the immediate base's type would miss
        // that `p` is transitively a member of the gsl::Pointer `v`. Peel
        // anonymous-record member accesses to reach the real enclosing object.
        const Expr *GslBase = Base;
        while (const auto *BME = dyn_cast<MemberExpr>(GslBase->IgnoreImpCasts())) {
          const auto *BFD = dyn_cast<FieldDecl>(BME->getMemberDecl());
          const RecordType *RT =
              BFD ? BFD->getType()->getAs<RecordType>() : nullptr;
          if (RT && RT->getDecl()->isAnonymousStructOrUnion())
            GslBase = BME->getBase();
          else
            break;
        }
        // Soundness: when the enclosing object is itself a view (gsl::Pointer) --
        // a leaf in the origin tree whose members are not tracked per field -- a
        // store into its borrow-holding member otherwise lands on a transient
        // member-access origin disconnected from the object, so the object never
        // reflects the stored borrow and a later read of it dangles silently.
        // Also MERGE the stored value's loans into the object's own origin (do
        // NOT kill -- the object may hold other borrows), so the view now
        // carries the borrow and a use of it after that borrow's source expires
        // is reported.
        //
        // The member may not be declared directly in the receiver's static type:
        // it can live in a [[gsl::Pointer]] BASE CLASS of a non-gsl derived
        // receiver (`struct D : ViewBase {}; d.p = &local`) and/or in an
        // ANONYMOUS struct/union nested inside one. Find the member's enclosing
        // gsl::Pointer by walking its declaring record outward through enclosing
        // anonymous records; the leaf treatment applies whenever such a record
        // exists. Exclude the case where that gsl::Pointer is the receiver's own
        // static class (a direct/anon member of a gsl::Pointer receiver), which
        // the receiver-type test below already handles -- merging there would
        // change its established (dangling-field) diagnostic.
        const RecordDecl *GslEnclosing = nullptr;
        for (const RecordDecl *R = LF->getParent(); R;) {
          if (isGslPointerType(AC.getASTContext().getCanonicalTagType(R))) {
            GslEnclosing = R;
            break;
          }
          if (!R->isAnonymousStructOrUnion())
            break;
          R = dyn_cast_or_null<RecordDecl>(R->getDeclContext());
        }
        QualType RecvTy =
            GslBase->IgnoreImpCasts()->getType().getNonReferenceType();
        if (RecvTy->isPointerType())
          RecvTy = RecvTy->getPointeeType();
        const RecordDecl *RecvClass = RecvTy->getAsRecordDecl();
        bool MemberInGslPointerBase =
            GslEnclosing && RecvClass &&
            GslEnclosing->getCanonicalDecl() != RecvClass->getCanonicalDecl();
        if (isGslPointerType(
                GslBase->IgnoreImpCasts()->getType().getNonReferenceType()) ||
            MemberInGslPointerBase) {
          // Merge into the OUTERMOST tracked object, not the immediate base.
          // For a nested store like `v.inner.q = &local` (where both `Outer v`
          // and its `Inner inner` member are gsl::Pointer leaves), `Container`
          // is the transient member-access origin of `v.inner`, disconnected
          // from `v`. Reads of the whole object route to `v` (leaf members are
          // untracked), so a merge into `v.inner` would never be seen. Each
          // such leaf member-access origin records its base as its parent (see
          // OriginManager::getOrCreateNode), so climbing the parent chain
          // reaches the outermost enclosing object regardless of how the access
          // is spelled (parens, implicit casts, arbitrarily deep member
          // chains) -- this is origin-tree-based, not AST-shape matching.
          OriginNode *MergeTarget = Container;
          while (OriginNode *P = MergeTarget->getParent())
            MergeTarget = P;
          // The merge is only observable if reads of the enclosing object
          // re-resolve to `MergeTarget`. That holds when it is anchored to a
          // declaration or `this`. If instead the object roots in a transient
          // selecting/forwarding expression -- `(c ? a.v : b.v).q = &local`, a
          // comma, a call result, or a temporary -- the borrow merged here is
          // dropped (reads of the real objects route elsewhere), so reject the
          // store as unroutable (the same one-way-merge limitation handled for
          // `!LHSNode` above), rather than tracking it unsoundly.
          OriginNode *MergePointee = MergeTarget->getPointeeChild();
          if (FactMgr.getOriginMgr().isStableStorageOrigin(MergeTarget) &&
              MergePointee && MergePointee->getLength() == RHSNode->getLength())
            flow(MergePointee, RHSNode, /*Kill=*/false);
          else if (hasOrigins(LHSExpr->getType()))
            // Either the object is a transient selecting base, or the stored
            // value's pointee chain does not match the view's (a multi-level
            // indirection member such as a `const char**` member of a
            // gsl::Pointer leaf -- which the single-borrow view model cannot
            // track and which would otherwise trip flow()'s length invariant).
            // Flag the unsupported store rather than tracking it unsoundly.
            CurrentBlockFacts.push_back(
                FactMgr.createFact<UntrackedConstructFact>(
                    UntrackedConstructReason::UnsupportedStoreDestination,
                    cast<Expr>(LHSExpr)));
        }
      }
    }
}

void FactsGenerator::handlePointerArithmetic(const BinaryOperator *BO) {
  if (Expr *RHS = BO->getRHS(); RHS->getType()->isPointerType()) {
    killAndFlowOrigin(*BO, *RHS);
    return;
  }
  Expr *LHS = BO->getLHS();
  assert(LHS->getType()->isPointerType() &&
         "Pointer arithmetic must have a pointer operand");
  killAndFlowOrigin(*BO, *LHS);
}

void FactsGenerator::VisitBinaryOperator(const BinaryOperator *BO) {
  if (BO->getOpcode() == BO_PtrMemD || BO->getOpcode() == BO_PtrMemI) {
    // Pointer-to-data-member access `obj.*pm` (BO_PtrMemD) / `objptr->*pm`
    // (BO_PtrMemI): the result is a glvalue naming a member of the object, so a
    // borrow of it borrows the object (`&(obj.*pm)` aims into `obj`). Flow the
    // object operand's origin into the result, mirroring a MemberExpr; without
    // this the borrow of the object is silently dropped to an empty origin (and
    // a control-flow merge supplying a valid loan could mask the lost-loan
    // backstop). For `.*` the object is the LHS; for `->*` it is what the LHS
    // pointer points at (its rvalue/pointee origin).
    OriginNode *Dst = getOriginNode(*BO);
    OriginNode *ObjSrc =
        BO->getOpcode() == BO_PtrMemD
            ? getOriginNode(*BO->getLHS())
            : getRValueOrigins(BO->getLHS(), getOriginNode(*BO->getLHS()));
    if (Dst && ObjSrc && Dst->getLength() == ObjSrc->getLength())
      flow(Dst, ObjSrc, /*Kill=*/true);
    handleUse(BO->getLHS());
    return;
  }
  if (BO->getOpcode() == BO_Comma) {
    // The comma operator's value is its right operand, so the result carries the
    // RHS's loans. Without this a borrow used via a comma result (e.g.
    // `g = (f(), p)`) is dropped. (For a non-pointer-like RHS there are no
    // origins to flow; killAndFlowOrigin is a no-op then.)
    killAndFlowOrigin(*BO, *BO->getRHS());
    return;
  }
  if (BO->isCompoundAssignmentOp()) {
    // A pointer compound additive assignment (`p += n` / `p -= n`) keeps the
    // pointer aimed into the same allocation, so its result (an lvalue
    // referring to the LHS) carries the LHS pointer's loans. Propagate them so
    // the result expression's origin is not empty -- mirroring
    // handlePointerArithmetic for the non-compound `p + n`. Without this a
    // borrow used via the compound-assign result (e.g. `g = (p += 5)`) is
    // dropped. Compound assignments on non-pointer types have no origins.
    if (BO->getType()->isPointerType())
      killAndFlowOrigin(*BO, *BO->getLHS());
    return;
  }
  if (BO->getType()->isPointerType() && BO->isAdditiveOp())
    handlePointerArithmetic(BO);
  handleUse(BO->getRHS());
  if (BO->isAssignmentOp())
    handleAssignment(BO, BO->getLHS(), BO->getRHS());
  // TODO: Handle assignments involving dereference like `*p = q`.
}

void FactsGenerator::VisitConditionalOperator(const ConditionalOperator *CO) {
  if (!hasOrigins(CO))
    return;

  const Expr *TrueExpr = CO->getTrueExpr();
  const Expr *FalseExpr = CO->getFalseExpr();

  const auto Preds = CurrentBlock->preds();

  // Skip origin flow from conditional operator arms that cannot produce the
  // result value: throw arms and calls to noreturn functions.
  bool TBHasEdge = true;
  bool FBHasEdge = true;

  switch (CurrentBlock->pred_size()) {
  case 0:
    return;
  case 1: {
    TBHasEdge = llvm::any_of(**Preds.begin(),
                             [ExpectedStmt = TrueExpr->IgnoreParenImpCasts()](
                                 const CFGElement &Elt) {
                               if (auto CS = Elt.getAs<CFGStmt>())
                                 return CS->getStmt() == ExpectedStmt;
                               return false;
                             });
    FBHasEdge = !TBHasEdge;
    break;
  }
  case 2: {
    const auto *It = Preds.begin();
    TBHasEdge = It->isReachable();
    FBHasEdge = (++It)->isReachable();
    break;
  }
  default:
    llvm_unreachable("expected at most 2 predecessors");
    return;
  }

  bool FirstFlow = true;
  auto HandleFlow = [&](const Expr *E) {
    if (FirstFlow) {
      killAndFlowOrigin(*CO, *E);
      FirstFlow = false;
    } else {
      flowOrigin(*CO, *E);
    }
  };

  if (TBHasEdge)
    HandleFlow(TrueExpr);
  if (FBHasEdge)
    HandleFlow(FalseExpr);
}

void FactsGenerator::VisitBinaryConditionalOperator(
    const BinaryConditionalOperator *BCO) {
  if (!hasOrigins(BCO))
    return;
  // The GNU binary conditional `a ?: b` yields `a` when `a` is truthy, else `b`.
  // Merge both candidate values' loans into the result (a conservative union),
  // so a borrow produced by either is tracked -- the common subexpression is the
  // "true" value (its OpaqueValueExpr forwards to it in getOrCreateNode).
  killAndFlowOrigin(*BCO, *BCO->getCommon());
  flowOrigin(*BCO, *BCO->getFalseExpr());
}

void FactsGenerator::VisitCXXOperatorCallExpr(const CXXOperatorCallExpr *OCE) {
  // Assignment operators have special "kill-then-propagate" semantics
  // and are handled separately.
  if (OCE->getOperator() == OO_Equal && OCE->getNumArgs() == 2 &&
      hasOrigins(OCE->getArg(0)->getType())) {
    // Pointer-like types: assignment inherently propagates origins.
    QualType LHSTy = OCE->getArg(0)->getType();
    if (LHSTy->isPointerOrReferenceType() || isGslPointerType(LHSTy) ||
        isGslOwnerType(LHSTy)) {
      handleAssignment(OCE, OCE->getArg(0), OCE->getArg(1));
      return;
    }
    // Standard library callable wrappers (e.g., std::function) can propagate
    // the stored lambda's origins.
    if (const auto *RD = LHSTy->getAsCXXRecordDecl();
        RD && isStdCallableWrapperType(RD)) {
      handleAssignment(OCE, OCE->getArg(0), OCE->getArg(1));
      return;
    }
    // Other tracked types: only defaulted operator= propagates origins.
    // User-defined operator= has opaque semantics, so don't handle them now.
    if (const auto *MD =
            dyn_cast_or_null<CXXMethodDecl>(OCE->getDirectCallee());
        MD && MD->isDefaulted()) {
      handleAssignment(OCE, OCE->getArg(0), OCE->getArg(1));
      return;
    }
  }

  ArrayRef Args = {OCE->getArgs(), OCE->getNumArgs()};
  // For `static operator()`, the first argument is the object argument,
  // remove it from the argument list to avoid off-by-one errors.
  if (OCE->getOperator() == OO_Call && OCE->getDirectCallee()->isStatic())
    Args = Args.slice(1);
  handleFunctionCall(OCE, OCE->getDirectCallee(), Args);
}

void FactsGenerator::VisitCXXFunctionalCastExpr(
    const CXXFunctionalCastExpr *FCE) {
  // Check if this is a test point marker. If so, we are done with this
  // expression.
  if (handleTestPoint(FCE))
    return;
  VisitCastExpr(FCE);
}

void FactsGenerator::VisitInitListExpr(const InitListExpr *ILE) {
  if (!hasOrigins(ILE))
    return;
  // Aggregate initialization of an array of pointer-like elements
  // (`int* arr[] = {&x, &y}`). All elements share the array's single
  // element-origin (indices are not disambiguated), so merge every element
  // initializer's loans into the list origin. A borrow stored into any element
  // is then visible through every element access.
  if (ILE->getType()->isArrayType()) {
    OriginNode *ListNode = getOriginNode(*ILE);
    if (!ListNode)
      return;
    bool First = true;
    for (const Expr *Init : ILE->inits()) {
      OriginNode *InitNode = getRValueOrigins(Init, getOriginNode(*Init));
      if (!InitNode)
        continue;
      // Kill on the first element to establish the set, merge the rest.
      flow(ListNode, InitNode, /*Kill=*/First);
      First = false;
    }
    return;
  }
  // For list initialization with a single element of the same type, like
  // `View{other}`, the origin of the list itself is the origin of its single
  // element.
  if (ILE->getNumInits() == 1 &&
      ILE->getType().getCanonicalType() ==
          ILE->getInit(0)->getType().getCanonicalType()) {
    killAndFlowOrigin(*ILE, *ILE->getInit(0));
    return;
  }
  // Aggregate initialization of a [[gsl::Pointer]] / [[gsl::Owner]] record from
  // its underlying members (`View{heap}`, `View{.p = local}`, `View{p, n}`).
  llvm::SmallVector<const Expr *, 4> Inits(ILE->inits().begin(),
                                           ILE->inits().end());
  handleGslAggregateInit(ILE, Inits);
  // Soundness: a plain (non-gsl) aggregate that holds a borrow but whose
  // ownership is untracked. A local/member declaration of such a type is
  // reported at the declaration (VisitDeclStmt) and a call result at the call,
  // but an aggregate *temporary* that escapes (`return Box{&x}`, `g =
  // Box{&x}.p`) is neither -- its borrow is orphaned and silently dropped. Flag
  // the escaping temporary here; skip the var/member-initializer form to avoid
  // double-reporting the declaration.
  maybeReportUntrackedAggregateTemporary(ILE);
}

void FactsGenerator::VisitCXXParenListInitExpr(
    const CXXParenListInitExpr *PLIE) {
  // The C++20 parenthesized aggregate init form `View(heap)`; same modeling as
  // the braced `View{heap}` InitListExpr.
  if (!hasOrigins(PLIE))
    return;
  llvm::SmallVector<const Expr *, 4> Inits(PLIE->getInitExprs().begin(),
                                           PLIE->getInitExprs().end());
  handleGslAggregateInit(PLIE, Inits);
  maybeReportUntrackedAggregateTemporary(PLIE);
}

void FactsGenerator::maybeReportUntrackedAggregateTemporary(
    const Expr *AggExpr) {
  // A [[gsl::Pointer]]/[[gsl::Owner]] aggregate is modeled (handleGslAggregate-
  // Init); only a plain borrow-holding aggregate is untracked here.
  QualType Ty = AggExpr->getType();
  if (isGslPointerType(Ty) || isGslOwnerType(Ty))
    return;
  // Skip an aggregate that directly initializes a declaration: a local/member
  // VarDecl is reported at the declaration (VisitDeclStmt) and reporting here
  // too would double-fire (the checker dedups per-Expr, not by location). The
  // initializer of a VarDecl appears, after peeling the implicit
  // ExprWithCleanups / Cast / paren wrappers, as that VarDecl's init. An
  // escaping temporary instead has a MaterializeTemporaryExpr, a ReturnStmt, or
  // some other expression as its parent.
  ParentMap &PM = AC.getParentMap();
  const Stmt *Cur = AggExpr;
  while (const Stmt *P = PM.getParent(Cur)) {
    // Walk up through wrappers and through an enclosing aggregate initializer
    // (a nested element init, `Outer{Inner{...}}`), so a sub-aggregate of a
    // declaration's initializer reaches the DeclStmt below and is skipped,
    // while a sub-aggregate of an escaping temporary reaches a ReturnStmt /
    // call / assignment and is still reported.
    if (isa<ExprWithCleanups>(P) || isa<ConstantExpr>(P) ||
        isa<ParenExpr>(P) || isa<CastExpr>(P) || isa<InitListExpr>(P) ||
        isa<CXXParenListInitExpr>(P)) {
      Cur = P;
      continue;
    }
    // A DeclStmt parent means this aggregate is a declaration's initializer
    // (`Box b{...};`), already reported at the VarDecl.
    if (isa<DeclStmt>(P))
      return;
    break;
  }
  // Mirror the 4-way classification used for local declarations (VisitDeclStmt)
  // and call results (handleFunctionCall): a borrow-holding type whose
  // ownership the analysis cannot track. Report the escaping aggregate temporary
  // so its orphaned borrow is not silently dropped.
  auto &Cache = FactMgr.getUnknownOwnershipCache();
  if (isUnknownOwnershipType(Ty, Cache))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::UnknownOwnership, AggExpr));
  else if (isGslOwnerOfIndirection(Ty, Cache))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::OwnerOfIndirection, AggExpr));
  else if (isGslPointerOfIndirection(Ty, Cache))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::PointerOfIndirection, AggExpr));
  else {
    bool IsPointer = false;
    if (QualType Nested =
            findNestedOwnerOrPointerOfIndirection(Ty, Cache, IsPointer);
        !Nested.isNull())
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          IsPointer ? UntrackedConstructReason::PointerOfIndirection
                    : UntrackedConstructReason::OwnerOfIndirection,
          AggExpr, Nested));
  }
}

void FactsGenerator::handleGslAggregateInit(
    const Expr *AggExpr, llvm::ArrayRef<const Expr *> Inits) {
  // A [[gsl::Pointer]] / [[gsl::Owner]] record is a LEAF in the origin tree
  // (its members are not tracked per field): the whole object carries a single
  // borrow, so merge every borrow-carrying initializer's loans into the
  // aggregate's own origin. Without this the aggregate's origin stays empty and
  // a borrow captured into a member is silently dropped -- and when the
  // destination object already holds a loan (e.g. a `this`/parameter
  // caller-scope placeholder that never expires), the lost-loan backstop is
  // masked, so a later dangling use of the member is missed entirely.
  if (!isGslPointerType(AggExpr->getType()) &&
      !isGslOwnerType(AggExpr->getType()))
    return;
  OriginNode *Dst = getRValueOrigins(AggExpr, getOriginNode(*AggExpr));
  if (!Dst)
    return;
  // Map initializers to fields so a reference member can be handled specially.
  // The inits are in subobject order: base-subobject initializers first (in
  // base declaration order), then fields (Sema reorders designated inits and
  // fills defaults). An aggregate's bases are always public and non-virtual, so
  // the leading `getNumBases()` inits are the bases; skip past them to align the
  // field iterator. (Brace elision only affects nested subaggregate members,
  // not this base/field split.)
  const CXXRecordDecl *RD = AggExpr->getType()->getAsCXXRecordDecl();
  bool CanZipFields = RD && !RD->isUnion();
  unsigned NumBaseInits = CanZipFields ? RD->getNumBases() : 0;
  RecordDecl::field_iterator FieldIt =
      RD ? RD->field_begin() : RecordDecl::field_iterator();
  RecordDecl::field_iterator FieldEnd =
      RD ? RD->field_end() : RecordDecl::field_iterator();
  bool First = true;
  unsigned Index = 0;
  for (const Expr *Init : Inits) {
    // A base-subobject initializer has no matching field; flow it as an rvalue
    // (a base may itself be a [[gsl::Pointer]] carrying a borrow) without
    // advancing the field iterator.
    const FieldDecl *FD = nullptr;
    if (Index++ >= NumBaseInits && CanZipFields && FieldIt != FieldEnd) {
      FD = *FieldIt;
      ++FieldIt;
    }
    // A reference member (`const T& r`) binds to the initializer's lvalue
    // storage -- a borrow of it, like `&lvalue`. The init expression is the
    // referent glvalue, so getRValueOrigins would peel its storage origin away
    // (a non-pointer referent then has no rvalue origin and the borrow is
    // dropped, silently when a sibling member's loan masks the empty origin).
    // Use the unpeeled storage origin instead, so the leaf view carries the
    // reference's borrow.
    bool RefMember = FD && FD->getType()->isReferenceType();
    OriginNode *InitNode = RefMember
                               ? getOriginNode(*Init)
                               : getRValueOrigins(Init, getOriginNode(*Init));
    // Only flow an initializer of matching indirection depth (a borrow the leaf
    // object holds); a non-pointer member (`unsigned n`) has no origin, and a
    // depth mismatch is handled by the multi-level-indirection rule elsewhere.
    if (!InitNode || InitNode->getLength() != Dst->getLength())
      continue;
    // Merge (not kill) across multiple borrow-carrying members; the first
    // establishes the set.
    flow(Dst, InitNode, /*Kill=*/First);
    First = false;
  }
}

void FactsGenerator::VisitCXXBindTemporaryExpr(
    const CXXBindTemporaryExpr *BTE) {
  killAndFlowOrigin(*BTE, *BTE->getSubExpr());
}

void FactsGenerator::VisitMaterializeTemporaryExpr(
    const MaterializeTemporaryExpr *MTE) {
  assert(MTE->isGLValue());
  OriginNode *MTENode = getOriginNode(*MTE);
  if (!MTENode)
    return;
  OriginNode *SubExprNode = getOriginNode(*MTE->getSubExpr());
  assert((!SubExprNode ||
          MTENode->getLength() == (SubExprNode->getLength() + 1)) &&
         "MTE top level origin should contain a loan to the MTE itself");

  OriginNode *RValMTENode = getRValueOrigins(MTE, MTENode);
  flow(RValMTENode, SubExprNode, /*Kill=*/true);
  OriginID OuterMTEID = MTENode->getOriginID();
  if (MTE->getStorageDuration() == SD_FullExpression) {
    // Issue a loan to MTE for the storage location represented by MTE.
    const Loan *L = createLoan(FactMgr, MTE);
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), OuterMTEID));
  }
}

void FactsGenerator::VisitLambdaExpr(const LambdaExpr *LE) {
  // The lambda gets a single merged origin that aggregates all captured
  // pointer-like origins. Currently we only need to detect whether the lambda
  // outlives any capture.
  OriginNode *LambdaNode = getOriginNode(*LE);
  if (!LambdaNode)
    return;
  bool Kill = true;
  const LambdaExpr::capture_iterator CapIt = LE->capture_begin();
  unsigned CapIdx = 0;
  for (const Expr *Init : LE->capture_inits()) {
    const LambdaCapture &Cap = *(CapIt + CapIdx);
    ++CapIdx;
    // Soundness: a by-reference capture of an indirection-typed variable (e.g.
    // `[&sv]` where sv is a std::string_view) forms a reference to a view -- two
    // levels of indirection. The lambda body reassigning the view is not flowed
    // back into the captured variable (the merged-origin model below tracks only
    // whether the lambda outlives a capture), so a borrow it holds can dangle
    // undetected. Reject such a capture, mirroring the single-indirection rule
    // for a `string_view&` declaration. Capturing an owner or a scalar by
    // reference is a single level and stays fine.
    if (Cap.getCaptureKind() == LCK_ByRef && Cap.capturesVariable() && Init)
      if (FactMgr.getOriginMgr().getIndirectionDepth(
              Cap.getCapturedVar()->getType().getNonReferenceType()) >= 1)
        CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
            UntrackedConstructReason::LambdaRefCaptureIndirection, Init));
    OriginNode *InitNode = nullptr;
    if (Cap.getCaptureKind() == LCK_This) {
      // A `[this]` capture stores a borrow of the enclosing object into the
      // closure. Flow the `this` origin (which carries the object's loan) into
      // the lambda so an escaping this-capturing closure is caught by the escape
      // machinery -- and a co-captured benign loan cannot mask it. The `this`
      // CXXThisExpr's own origin node is empty (the seeded loan lives on the
      // dedicated this-origin), so use that directly.
      if (auto ThisOrigins = FactMgr.getOriginMgr().getThisOrigins())
        InitNode = *ThisOrigins;
    } else if (Init) {
      InitNode = getOriginNode(*Init);
    }
    if (!InitNode)
      continue;
    // FIXME: Consider flowing all origin levels once lambdas support more than
    // one origin. Currently only the outermost origin is flowed, so by-ref
    // captures like `[&p]` (where p is string_view) miss inner-level
    // invalidation.
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        LambdaNode->getOriginID(), InitNode->getOriginID(), Kill));
    Kill = false;
  }
}

void FactsGenerator::VisitArraySubscriptExpr(const ArraySubscriptExpr *ASE) {
  assert(ASE->isGLValue() && "Array subscript should be a GL value");
  OriginNode *Dst = getOriginNode(*ASE);
  assert(Dst && "Array subscript should have origins as it is a GL value");
  OriginNode *Src = getOriginNode(*ASE->getBase());
  assert(Src && "Base of array subscript should have origins");
  CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
      Dst->getOriginID(), Src->getOriginID(), /*Kill=*/true));
}

bool FactsGenerator::handlePlacementNew(const CXXNewExpr *NE,
                                        OriginNode *NewNode) {
  // Model the standard non-allocating placement-new form, whose allocation
  // function takes the storage as a `void*` parameter (the first parameter
  // after the size) and returns it. Extra placement arguments (tag types,
  // allocator state, etc.) may follow -- e.g. `new (buf, Tag{}) T` -- but the
  // first placement argument is still the storage buffer, so forward its loan
  // regardless of how many placement arguments there are. An allocating
  // placement form (e.g. `new (std::nothrow) T`, whose first placement
  // parameter is not `void*`) genuinely allocates fresh storage and is handled
  // by the caller.
  if (NE->getNumPlacementArgs() < 1)
    return false;

  const FunctionDecl *OperatorNew = NE->getOperatorNew();
  if (OperatorNew->getNumParams() <= 1)
    return false;

  const auto *Arg =
      OperatorNew->getParamDecl(1)->getType()->getAs<PointerType>();
  if (!Arg || !Arg->isVoidPointerType())
    return false;

  // Use the placement argument before the implicit conversion to void*, so
  // inner origins are still available.
  const Expr *PlacementArg = NE->getPlacementArg(0);
  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(PlacementArg);
      ICE && ICE->getCastKind() == CK_BitCast &&
      PlacementArg->getType()->isVoidPointerType())
    PlacementArg = ICE->getSubExpr();
  OriginNode *PlacementNode = getOriginNode(*PlacementArg);

  // The pointer returned by placement new comes from the placement
  // argument.
  if (PlacementNode)
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        NewNode->getOriginID(), PlacementNode->getOriginID(), true));
  return true;
}

void FactsGenerator::VisitCXXNewExpr(const CXXNewExpr *NE) {
  OriginNode *NewNode = getOriginNode(*NE);
  const Expr *Init = NE->getInitializer();

  // A non-allocating placement-new forwards its buffer's loan (so freeing the
  // buffer dangles a borrow into the placed object). Any other new -- a plain
  // allocating new, or an allocating placement form (nothrow) -- gets a fresh
  // heap-allocation loan. (A single non-void* placement arg, e.g. nothrow, is
  // left with no loan here so a later use trips the lost-loan backstop, the
  // prior behavior.)
  if (!handlePlacementNew(NE, NewNode) && NE->getNumPlacementArgs() != 1) {
    const Loan *L = createLoan(FactMgr, NE);
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), NewNode->getOriginID()));
  }

  NewNode = NewNode->getPointeeChild();

  if (!NewNode || !Init)
    return;

  // FIXME: OriginNode is null for `new[]` initializers. Remove this `Init`
  // check once array origins are supported.
  if (OriginNode *InitNode = getOriginNode(*Init); InitNode)
    flow(NewNode, InitNode, true);
}

void FactsGenerator::VisitCXXDeleteExpr(const CXXDeleteExpr *DE) {
  OriginNode *Node = getOriginNode(*DE->getArgument());
  CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
      Node->getOriginID(), DE, /*Assumed=*/false, /*Deallocation=*/true));
}

void FactsGenerator::VisitCXXThrowExpr(const CXXThrowExpr *TE) {
  // Exception control flow (stack unwinding, running destructors and resuming
  // in a handler) is not modeled, so a borrow that dangles only along an
  // exception path can be missed. Surface the `throw` as an unsupported
  // construct.
  CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
      UntrackedConstructReason::Exception, TE->getThrowLoc()));
}

void FactsGenerator::VisitStmtExpr(const StmtExpr *SE) {
  // A statement expression's value is its final expression, whose origin
  // getOrCreateNode forwards to. Mark that value as USED here -- at the
  // statement-expression's own program point, which the CFG places AFTER the
  // body's locals have expired. This keeps a borrow the value carries live
  // across those expiries, so a borrow of a body-local escaping via the value
  // is reported as a use-after-scope rather than silently dropped (the
  // in-transit loan would otherwise not be "live" at the local's expiry).
  handleUse(SE);
}

void FactsGenerator::VisitGCCAsmStmt(const GCCAsmStmt *AS) {
  // Inline assembly is opaque to the analysis: an output operand can reseat a
  // pointer to anything (so a stale loan on it would be wrongly trusted), and an
  // input or memory-clobbering operand can move or invalidate a borrow, with no
  // modeled flow. Reject the construct under the safe programming model.
  CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
      UntrackedConstructReason::InlineAsm, AS->getAsmLoc()));
}

void FactsGenerator::VisitMSAsmStmt(const MSAsmStmt *AS) {
  // Microsoft-style `__asm` blocks are equally opaque; reject them too.
  CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
      UntrackedConstructReason::InlineAsm, AS->getAsmLoc()));
}

void FactsGenerator::handleTryStatements() {
  const Stmt *Body = AC.getBody();
  if (!Body)
    return;
  // Shallow worklist over the body's statements. Do not descend into nested
  // lambdas/blocks; their bodies are separate functions analyzed on their own.
  llvm::SmallVector<const Stmt *, 32> Worklist{Body};
  while (!Worklist.empty()) {
    const Stmt *S = Worklist.pop_back_val();
    if (!S || isa<LambdaExpr>(S))
      continue;
    // Any construct that introduces exception-handling / unwinding control flow
    // the analysis does not model. The handler resumes after the stack has
    // unwound (with destructors having run), an edge the CFG does not carry, so
    // a borrow that dangles only along that path would be silently missed.
    // This mirrors VisitCXXThrowExpr for bare C++ `throw`, but is spelled as an
    // AST walk so it also covers constructs that may not surface as their own
    // CFG element: C++ `try`, Objective-C `@try`/`@throw`, and SEH `__try`.
    SourceLocation ExcLoc;
    if (const auto *TS = dyn_cast<CXXTryStmt>(S))
      ExcLoc = TS->getTryLoc();
    else if (const auto *OT = dyn_cast<ObjCAtTryStmt>(S))
      ExcLoc = OT->getAtTryLoc();
    else if (const auto *OT = dyn_cast<ObjCAtThrowStmt>(S))
      ExcLoc = OT->getThrowLoc();
    else if (const auto *ST = dyn_cast<SEHTryStmt>(S))
      ExcLoc = ST->getTryLoc();
    if (ExcLoc.isValid())
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::Exception, ExcLoc));
    Worklist.append(S->child_begin(), S->child_end());
  }
}

bool FactsGenerator::escapesViaReturn(OriginID OID) const {
  return llvm::any_of(EscapesInCurrentBlock, [OID](const Fact *F) {
    if (const auto *EF = F->getAs<ReturnEscapeFact>())
      return EF->getEscapedOriginID() == OID;
    return false;
  });
}

void FactsGenerator::handleLifetimeEnds(const CFGLifetimeEnds &LifetimeEnds) {
  const VarDecl *LifetimeEndsVD = LifetimeEnds.getVarDecl();
  if (!LifetimeEndsVD)
    return;
  // A non-trivial destructor running at scope exit may read a borrow the object
  // holds (e.g. a [[gsl::Pointer]] whose ~T() dereferences its captured view).
  // The analysis is intra-procedural and does not see the out-of-line destructor
  // body, so model the destruction as a *use* of the object: this keeps the
  // borrow live up to the destruction point, so that a borrowed-from object
  // destroyed earlier (reverse-declaration-order) is reported at its expiry.
  // Owners are excluded -- their destruction frees their own storage (already
  // modeled by the ExpireFact) rather than dereferencing a borrow into another
  // object. A trivial destructor cannot read the borrow.
  QualType VDTy = LifetimeEndsVD->getType();
  if (const CXXRecordDecl *RD = VDTy->getAsCXXRecordDecl();
      RD && RD->hasDefinition() && RD->hasNonTrivialDestructor() &&
      !isGslOwnerType(VDTy) && hasOrigins(VDTy))
    if (OriginNode *Node = getOriginNode(*LifetimeEndsVD)) {
      CurrentBlockFacts.push_back(FactMgr.createFact<UseFact>(
          LifetimeEnds.getTriggerStmt()->getEndLoc(), Node));
      // Soundness: a gsl::Pointer object that captured a mutable owner (e.g. an
      // RAII guard `Trigger(MyOwner * [[clang::lifetime_capture_by(this)]])`
      // whose `~Trigger() { o->grow(); }` reallocates it) may mutate/free that
      // owner in its out-of-line destructor, which the intra-procedural analysis
      // cannot see. Conservatively treat the destruction as an assumed
      // invalidation of the borrows the object carries on its captured owner
      // (its pointee chain), so a view into that owner that is live *past* the
      // guard's scope is reported -- while a view used only during the guard's
      // lifetime (before the destructor runs) is not. Gated on paramMayMutateOwner
      // (the same "gsl::Pointer reaching a mutable owner" test used for call
      // arguments), so a guard that aliases only a const owner is not flagged.
      // The invalidating "operation" here is the destructor's trigger
      // statement, not an expression -- hence InvalidateOriginFact carries a
      // Stmt. A gsl::Pointer is a leaf in the origin tree, so the captured
      // borrow sits on the object's OWN origin; invalidate it and its whole
      // pointee chain (the latter for a nested wrapper reaching the owner
      // through a by-value gsl::Pointer member).
      if (paramMayMutateOwner(VDTy)) {
        auto invalidate = [&](OriginID OID) {
          CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
              OID, LifetimeEnds.getTriggerStmt(), /*Assumed=*/true));
        };
        invalidate(Node->getOriginID());
        for (OriginNode *Pointee = Node->getPointeeChild(); Pointee;
             Pointee = Pointee->getPointeeChild())
          invalidate(Pointee->getOriginID());
      }
    }
  // Expire the origin when its variable's lifetime ends to ensure liveness
  // doesn't persist through loop back-edges.
  std::optional<OriginID> ExpiredOID;
  if (OriginNode *Node = getOriginNode(*LifetimeEndsVD)) {
    OriginID OID = Node->getOriginID();
    // Skip origins that escape via return; the escape checker needs their loans
    // to remain until the return statement is processed.
    if (!escapesViaReturn(OID))
      ExpiredOID = OID;
  }
  CurrentBlockFacts.push_back(FactMgr.createFact<ExpireFact>(
      AccessPath(LifetimeEndsVD), LifetimeEnds.getTriggerStmt()->getEndLoc(),
      ExpiredOID));
}

void FactsGenerator::handleCleanupFunction(
    const CFGCleanupFunction &CleanupFunction) {
  // A variable with `__attribute__((cleanup(fn)))` has `fn(&var)` called at
  // scope exit. Like a non-trivial destructor, that callback may read a borrow
  // the variable holds (e.g. a [[gsl::Pointer]] whose cleanup dereferences its
  // captured view). The analysis does not see the out-of-line callback body, so
  // model the cleanup call as a *use* of the variable: this keeps the borrow
  // live up to the cleanup point, so a borrowed-from object destroyed earlier
  // (reverse-declaration order) is reported at its expiry. The CFG emits this
  // element after the variable's lifetime-end run, interleaved with the other
  // scope-exit cleanups in reverse construction order.
  const VarDecl *VD = CleanupFunction.getVarDecl();
  if (!VD || !hasOrigins(VD->getType()))
    return;
  if (OriginNode *Node = getOriginNode(*VD))
    CurrentBlockFacts.push_back(FactMgr.createFact<UseFact>(
        VD->getLocation(), Node));
}

void FactsGenerator::handleFullExprCleanup(
    const CFGFullExprCleanup &FullExprCleanup) {
  for (const auto *MTE : FullExprCleanup.getExpiringMTEs())
    CurrentBlockFacts.push_back(FactMgr.createFact<ExpireFact>(
        AccessPath(MTE), FullExprCleanup.getCleanupLoc()));
}

void FactsGenerator::handleExitBlock() {
  for (const Origin &O : FactMgr.getOriginMgr().getOrigins())
    if (auto *FD = dyn_cast_if_present<FieldDecl>(O.getDecl()))
      // Create FieldEscapeFacts for all field origins that remain live at exit.
      EscapesInCurrentBlock.push_back(
          FactMgr.createFact<FieldEscapeFact>(O.ID, FD));
    else if (auto *VD = dyn_cast_if_present<VarDecl>(O.getDecl())) {
      // Create GlobalEscapeFacts for all origins with global-storage that
      // remain live at exit.
      if (VD->hasGlobalStorage()) {
        EscapesInCurrentBlock.push_back(
            FactMgr.createFact<GlobalEscapeFact>(O.ID, VD));
      }
    }

  // Soundness: keep the implicit object (`this`) origin live at function exit.
  // A borrow captured via [[clang::lifetime_capture_by(this)]] is modeled as a
  // flow into the whole-object `this` origin (we do not know which member holds
  // it), and `this` is a caller-scope placeholder that never expires -- so when
  // the capture, the captured local's expiry, and a later read all happen inside
  // one method, the captured local's loan sits on an origin that is not
  // otherwise live at the expiry and the dangle is missed. An implicit use of
  // `this` here makes it live back through the expiry, so checkExpiry reports the
  // captured local going out of scope while still held by the object. The `this`
  // placeholder loan never expires, so this adds no false positive for an object
  // that only holds caller-scoped borrows.
  if (auto ThisOrigins = FactMgr.getOriginMgr().getThisOrigins())
    CurrentBlockFacts.push_back(FactMgr.createFact<UseFact>(
        AC.getDecl()->getEndLoc(), *ThisOrigins));
}

void FactsGenerator::handleGSLPointerConstruction(const CXXConstructExpr *CCE) {
  assert(isGslPointerType(CCE->getType()));
  if (CCE->getNumArgs() == 1) {
    const Expr *Arg = CCE->getArg(0);
    if (isGslPointerType(Arg->getType())) {
      OriginNode *ArgNode = getOriginNode(*Arg);
      assert(ArgNode && "GSL pointer argument should have an origin list");
      // GSL pointer is constructed from another gsl pointer.
      // Example:
      //  View(View v);
      //  View(const View &v);
      ArgNode = getRValueOrigins(Arg, ArgNode);
      flow(getOriginNode(*CCE), ArgNode, /*Kill=*/true);
      return;
    }
    if (Arg->getType()->isPointerType()) {
      // GSL pointer is constructed from a raw pointer. Flow only the outermost
      // raw pointer. Example:
      //  View(const char*);
      //  Span<int*>(const in**);
      OriginNode *ArgNode = getOriginNode(*Arg);
      CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
          getOriginNode(*CCE)->getOriginID(), ArgNode->getOriginID(),
          /*Kill=*/true));
      return;
    }
  }
  // Other constructions -- a multi-argument pointer/iterator form such as
  // `span(ptr, size)` or `span(first, last)`, or a single container/range
  // argument -- are handled by the general lifetimebound parameter->return
  // propagation in handleFunctionCall: the data/iterator parameter of the
  // standard views carries an (inferred) [[clang::lifetimebound]], so its borrow
  // flows into the constructed view. The range/container constructor parameter
  // is deliberately *not* lifetimebound, so `span(container)` is unaffected
  // here (it remains an unannotated indirection).
  handleFunctionCall(CCE, CCE->getConstructor(),
                     {CCE->getArgs(), CCE->getNumArgs()},
                     /*IsGslConstruction=*/true);
}

void FactsGenerator::handleMovedArgsInCall(const FunctionDecl *FD,
                                           ArrayRef<const Expr *> Args) {
  unsigned IsInstance = 0;
  if (const auto *MD = dyn_cast<CXXMethodDecl>(FD);
      MD && MD->isInstance() && !isa<CXXConstructorDecl>(FD)) {
    IsInstance = 1;
    // std::unique_ptr::release() transfers ownership.
    // Treat it as a move to prevent false-positive warnings when the unique_ptr
    // destructor runs after ownership has been transferred.
    if (isUniquePtrRelease(*MD)) {
      const Expr *UniquePtrExpr = Args[0];
      OriginNode *MovedOrigins = getOriginNode(*UniquePtrExpr);
      if (MovedOrigins)
        CurrentBlockFacts.push_back(FactMgr.createFact<MovedOriginFact>(
            UniquePtrExpr, MovedOrigins->getOriginID()));
    }
  }

  // Skip 'this' arg as it cannot be moved.
  for (unsigned I = IsInstance;
       I < Args.size() && I < FD->getNumParams() + IsInstance; ++I) {
    const ParmVarDecl *PVD = FD->getParamDecl(I - IsInstance);
    if (!PVD->getType()->isRValueReferenceType())
      continue;
    const Expr *Arg = Args[I];
    OriginNode *MovedOrigins = getOriginNode(*Arg);
    assert(MovedOrigins->getLength() >= 1 &&
           "unexpected length for r-value reference param");
    // Arg is being moved to this parameter. Mark the origin as moved.
    CurrentBlockFacts.push_back(
        FactMgr.createFact<MovedOriginFact>(Arg, MovedOrigins->getOriginID()));
  }
}

void FactsGenerator::handleInvalidatingCall(const Expr *Call,
                                            const FunctionDecl *FD,
                                            ArrayRef<const Expr *> Args) {
  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  if (!MD || !MD->isInstance())
    return;

  if (!isInvalidationMethod(*MD))
    return;

  // Accept a direct variable receiver (`v.clear()`); an owner-field receiver
  // (`this->buf.append(...)`), scoped via MutatedField since the receiver origin
  // also carries the enclosing object's loan; or any other owner-typed lvalue or
  // pointer-to-owner, e.g. a smart-/raw-pointer dereference or arrow
  // (`(*p).append(...)`, `p->append(...)`) whose origin carries the borrow the
  // mutation invalidates. A non-owner, non-variable receiver (e.g. a view
  // member) is skipped to avoid false positives.
  const Expr *Recv = Args[0]->IgnoreImpCasts();
  const FieldDecl *MutatedField = nullptr;
  if (const auto *ME = dyn_cast<MemberExpr>(Recv))
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
        FD && isMutableOwnerType(FD->getType()))
      MutatedField = FD;
  QualType RecvTy = Recv->getType();
  bool OwnerReceiver =
      isMutableOwnerType(RecvTy) ||
      (RecvTy->isPointerType() && isMutableOwnerType(RecvTy->getPointeeType()));
  if (!isa<DeclRefExpr>(Recv) && !MutatedField && !OwnerReceiver)
    return;

  OriginNode *ThisNode = getOriginNode(*Args[0]);
  if (ThisNode)
    CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
        ThisNode->getOriginID(), Call, /*Assumed=*/false,
        /*Deallocation=*/false, MutatedField));
}

// Soundness: the analysis conservatively assumes that operations it cannot
// prove leave an owner unchanged invalidate borrows into that owner. Two cases:
//   1. a non-const member call on an owner (other than known container mutators,
//      which are handled precisely, and borrow-returning accessors), and
//   2. passing an owner to a non-const pointer/reference parameter.
// These emit an *assumed* InvalidateOriginFact; the checker only warns when a
// borrow into the owner is actually live across the operation.
void FactsGenerator::handleAssumedInvalidatingCall(
    const Expr *Call, const FunctionDecl *FD, ArrayRef<const Expr *> Args) {
  const auto *Method = dyn_cast<CXXMethodDecl>(FD);
  bool IsInstance =
      Method && Method->isInstance() && !isa<CXXConstructorDecl>(FD);

  auto invalidate = [&](OriginID OID,
                        OwnerLoanGate LoanGate = OwnerLoanGate::None) {
    CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
        OID, Call, /*Assumed=*/true, /*Deallocation=*/false,
        /*MutatedField=*/nullptr, LoanGate));
  };
  // A gsl::Pointer object/argument carries the borrows it holds on its pointee
  // origin(s). Invalidate the *entire* pointee chain, not just the first level:
  // a nested gsl::Pointer wrapper (a gsl::Pointer whose member is itself a
  // by-value gsl::Pointer) reaches the borrowed owner several pointee levels
  // down, so a borrow taken directly from the aliased owner lives deeper than
  // getPointeeChild() once.
  auto invalidatePointeeChain =
      [&](OriginNode *L, OwnerLoanGate LoanGate = OwnerLoanGate::None) {
    for (OriginNode *Pointee = L->getPointeeChild(); Pointee;
         Pointee = Pointee->getPointeeChild())
      invalidate(Pointee->getOriginID(), LoanGate);
  };
  // Mutating an owner *through a member* (`w.in.grow()`) can invalidate a
  // borrow that flowed into an enclosing object rather than the receiver
  // itself. A gsl::Pointer / owner record is a leaf in the origin tree (its
  // members are not expanded -- see buildNodeForTypeImpl), so a borrow captured
  // into `w` at construction lives on `w`'s own origin and is *not* reachable
  // from the freshly built `w.in` receiver origin. The origin records that
  // enclosing object as its parent (set in getOrCreateNode), so walk the parent
  // chain and invalidate each enclosing object (outer + its whole pointee
  // chain). This is driven by the origin tree, not by AST pattern-matching the
  // base expression -- robust to forms like `(c ? a.f : b.f).g`. Invalidation
  // matches by exact borrowed-storage (AccessPath) identity, so an enclosing
  // object that holds no aliasing borrow yields nothing.
  auto invalidateEnclosingObjects =
      [&](OriginNode *L, OwnerLoanGate LoanGate = OwnerLoanGate::None) {
    for (OriginNode *P = L->getParent(); P; P = P->getParent()) {
      invalidate(P->getOriginID(), LoanGate);
      invalidatePointeeChain(P, LoanGate);
    }
  };

  // (1) Non-const member call whose receiver is an owner, or a non-owner object
  // that CONTAINS owner fields (e.g. `struct S { std::string buf; };`). The call
  // may reallocate the owner (or one of its owner fields), invalidating borrows
  // into it. Known container mutators are handled precisely elsewhere.
  //
  // A non-const call is conservatively assumed to invalidate -- returning a
  // borrow (lifetimebound / accessor) is orthogonal to mutating -- EXCEPT a
  // recognized non-invalidating accessor of a standard-library owner (`v[i]`,
  // `v.at(i)`, `v.data()`, `m.find(k)`, `*p`, ...). A const method cannot mutate
  // (`!isConst()`).
  if (IsInstance && !Method->isConst() && !isInvalidationMethod(*Method) &&
      !Args.empty()) {
    // Use the most-derived receiver type, not the static type at the call site.
    // The receiver's *static* type does not always reveal an owner the call may
    // mutate: a method may be reached through a base reference/pointer (`Base& b
    // = d; b.grow();`), a ternary, etc., whose static type is an owner-less base
    // while the receiver's loan actually points at a derived owner. So:
    //  - If the static type already shows an owner (or owner-containing record),
    //    or the call is a virtual dispatch on a polymorphic receiver (the
    //    dynamic type may add an owner), emit unconditionally (as before).
    //  - Otherwise, for any other record receiver, still emit -- but mark it
    //    OwnerLoanGate::ReachableOwner so the checker only acts when a loan the
    //    receiver actually carries points at a mutable owner. This is loan-based
    //    (what the receiver refers to), robust to references/pointers/ternaries.
    QualType RecvTy = Args[0]->IgnoreImpCasts()->getType().getNonReferenceType();
    bool OwnerReceiver = isGslOwnerType(RecvTy);
    // For an arrow receiver (`p->grow()`) the implicit object argument is the
    // pointer; peel it to reach the record (recordHasGslOwnerField already peels
    // it internally for the owner test).
    QualType RecvRecordTy =
        RecvTy->isPointerType() ? RecvTy->getPointeeType() : RecvTy;
    const CXXRecordDecl *RecvRD = RecvRecordTy->getAsCXXRecordDecl();
    bool MaybeDynamicOwner =
        Method->isVirtual() && RecvRD && RecvRD->isPolymorphic();
    bool StaticallyOwner =
        OwnerReceiver || recordHasGslOwnerField(RecvTy) || MaybeDynamicOwner;
    // A recognized non-invalidating accessor (`data()`, `front()`, `at()`,
    // `operator[]`, ...) does not reallocate, whether the owner is the direct
    // receiver (`v.data()`) or reached through a pointer (`p->data()`). Test the
    // pointee record so the allow-list applies in both cases -- otherwise a read
    // through a pointer-to-owner is spuriously treated as a mutation.
    bool PointeeIsOwner = isGslOwnerType(RecvRecordTy);
    if ((StaticallyOwner || RecvRD) &&
        !(PointeeIsOwner && isNonInvalidatingMethod(*Method)))
      if (OriginNode *L = getOriginNode(*Args[0])) {
        OwnerLoanGate RecvGate = StaticallyOwner ? OwnerLoanGate::None
                                                 : OwnerLoanGate::ReachableOwner;
        invalidate(L->getOriginID(), RecvGate);
        // When the receiver is a gsl::Pointer object (a view/wrapper that
        // reaches a mutable owner through what it points at, e.g. a wrapper
        // holding `std::vector<int>* v`), the borrows it carries also live on
        // its pointee origin(s). The outer invalidation above already covers a
        // borrow returned by a lifetimebound accessor of this object (which
        // carries the object's loan). Mirrors shouldTrackPointerImplicitObjectArg
        // in handleFunctionCall.
        if (isGslPointerType(Args[0]->getType().getNonReferenceType()))
          invalidatePointeeChain(L, RecvGate);
        invalidateEnclosingObjects(L, RecvGate);
      }
  }
  // The implicit object argument (I == 0 for implicit-this instance methods) is
  // intentionally skipped below -- it is handled by case (1) above. (For a C++23
  // explicit object member function the object IS a parameter, handled uniformly
  // by paramForArg; but its mutation as the receiver is still case (1), so skip
  // its parameter here to avoid double-flagging.)
  for (unsigned I = 0; I < Args.size(); ++I) {
    if (IsInstance && I == 0)
      continue; // object argument, handled above
    const ParmVarDecl *PVD = paramForArg(FD, IsInstance, I);
    if (!PVD)
      continue;
    // The parameter's *static* type does not always reveal an owner the call may
    // mutate: passing `*this` to a parameter typed as an abstract base erases the
    // reachability edge, while a virtual call inside the callee dispatches right
    // back to the derived object and can reallocate what it owns. So:
    //  - If the static type shows a mutable owner, emit unconditionally.
    //  - Otherwise, if the pointee is polymorphic (the dynamic type may add one),
    //    still emit -- but gate it (OwnerLoanGate::DenotedOwner) so the checker acts
    //    when a loan the argument actually carries points at a mutable owner.
    //    This mirrors the receiver branch in case (1) above and is loan-based, so
    //    an unrelated polymorphic argument that denotes no owner yields nothing.
    bool MayMutate = paramMayMutateOwner(PVD->getType());
    if (!MayMutate && !paramMayReachDynamicOwner(PVD->getType()))
      continue;
    if (OriginNode *L = getOriginNode(*Args[I])) {
      // A dynamic-owner-only argument must be confirmed to DENOTE the mutated
      // object (see OwnerLoanGate::DenotedOwner).
      OwnerLoanGate ArgGate =
          MayMutate ? OwnerLoanGate::None : OwnerLoanGate::DenotedOwner;
      invalidate(L->getOriginID(), ArgGate);
      // A gsl::Pointer argument (a view/wrapper that reaches a mutable owner
      // through what it points at) carries its borrows on the pointee
      // origin(s), so also invalidate the whole pointee chain -- mirroring the
      // receiver branch above -- so a borrow taken directly from the aliased
      // owner is invalidated too, at any nesting depth.
      if (isGslPointerType(PVD->getType().getNonReferenceType()))
        invalidatePointeeChain(L, ArgGate);
      // Enclosing objects only when the static type itself shows a mutable owner.
      // The dynamic-type case justifies "the callee may reallocate what the
      // ARGUMENT object owns" (via virtual dispatch on it), not what an enclosing
      // object owns: passing a field to a polymorphic callback (`drive(this->buf)`
      // where buf is-a Sink) cannot reach a disjoint sibling field, so walking the
      // parent chain there would flag borrows of siblings.
      if (MayMutate)
        invalidateEnclosingObjects(L);
    }
  }
}

void FactsGenerator::handleDestructiveCall(const Expr *Call,
                                           const FunctionDecl *FD,
                                           ArrayRef<const Expr *> Args) {
  if (!destructsFirstArg(*FD))
    return;
  OriginNode *ArgNode = getOriginNode(*Args[0]);
  if (ArgNode)
    CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
        ArgNode->getOriginID(), Call, /*Assumed=*/false,
        /*Deallocation=*/true));
}

// Soundness: detect overlapping (aliasing) call arguments. No lifetime
// annotation expresses that two arguments must not alias, so passing an owner
// the call may mutate together with a view that borrows it (`f(s, v)` with
// `string_view v = s;`) is a silent hazard: the callee may reallocate the owner
// and then use the dangling view, in an order the caller cannot see. For each
// argument the call may mutate, pair it with every other borrow-holding
// argument; the checker reports the pair when their loans actually alias.
void FactsGenerator::handleArgumentOverlap(const Expr *Call,
                                           const FunctionDecl *FD,
                                           ArrayRef<const Expr *> Args) {
  const auto *Method = dyn_cast<CXXMethodDecl>(FD);
  bool IsInstance =
      Method && Method->isInstance() && !isa<CXXConstructorDecl>(FD);

  // Whether the call may mutate the storage argument `I` refers to in a way
  // that reallocates/invalidates borrows into it -- i.e. it is (or contains) an
  // owner being mutated. A plain non-owner receiver (e.g. an element's
  // `operator=`) does not reallocate, so it is not an aliasing hazard.
  auto IsMutatingArg = [&](unsigned I) -> bool {
    if (IsInstance && I == 0) {
      // The receiver of a non-const instance method may be reallocated only if
      // it is an owner (or contains owner fields). Whether the method also
      // returns a borrow (lifetimebound / a GSL accessor) is orthogonal.
      // (I == 0 is only reached from the loop below, so Args[0] exists.)
      if (Method->isConst())
        return false;
      QualType RecvTy = Args[0]->getType();
      return isGslOwnerType(RecvTy) || recordHasGslOwnerField(RecvTy);
    }
    const ParmVarDecl *PVD = paramForArg(FD, IsInstance, I);
    return PVD && paramMayMutateOwner(PVD->getType());
  };

  for (unsigned M = 0; M < Args.size(); ++M) {
    if (!IsMutatingArg(M))
      continue;
    OriginNode *MutNode = getOriginNode(*Args[M]);
    if (!MutNode)
      continue;
    // Collect the other arguments that could alias the mutated owner into a
    // single fact for it (rather than one fact per pair).
    llvm::SmallVector<OriginID, 4> Borrows;
    for (unsigned B = 0; B < Args.size(); ++B) {
      if (B == M)
        continue;
      // A co-argument can alias the mutated owner's storage if it carries a
      // borrow into it. We rely on the propagated origins/loans rather than the
      // argument's static type, and the checker compares the actual loans -- so
      // collect both origin levels and let it decide: the OUTER origin (the
      // borrow is the handle itself -- a reference/pointer, or a
      // [[clang::lifetimebound]] result such as `h.get()` returning a reference
      // into `*h.p` while the receiver `h` is the mutated arg) and the RVALUE
      // origin (the borrow is the value -- a view such as string_view, whose
      // loans live one level in). Non-borrow co-arguments simply hold no
      // matching loan and are ignored by the checker.
      OriginNode *Outer = getOriginNode(*Args[B]);
      if (Outer)
        Borrows.push_back(Outer->getOriginID());
      if (OriginNode *RVal = getRValueOrigins(Args[B], Outer);
          RVal && RVal != Outer)
        Borrows.push_back(RVal->getOriginID());
    }
    if (Borrows.empty())
      continue;
    // The mutating origin together with its pointee chain: for a gsl::Pointer
    // argument the borrow into the aliased owner lives on the pointee origin
    // (the wrapper points AT the owner), not on the wrapper's own origin. Only
    // a gsl::Pointer indirects this way -- a plain owner / owner-containing
    // record holds its borrows on the top origin and its field children carry
    // unrelated loans -- so gate on it, mirroring invalidatePointeeChain in
    // handleAssumedInvalidatingCall.
    llvm::SmallVector<OriginID, 4> MutOrigins;
    MutOrigins.push_back(MutNode->getOriginID());
    if (isGslPointerType(Args[M]->getType().getNonReferenceType()))
      for (OriginNode *Pointee = MutNode->getPointeeChild(); Pointee;
           Pointee = Pointee->getPointeeChild())
        MutOrigins.push_back(Pointee->getOriginID());
    // The precise record being mutated is the argument's static type (the
    // actual subobject -- e.g. `Grid` for `world.grid_.build(...)`). The
    // mutating origin's loan may widen to the enclosing object's `$this`
    // placeholder, so the checker must NOT derive the mutated record from the
    // loan -- that would over-match disjoint sibling fields. Peel a pointer /
    // reference to reach the mutated pointee record.
    QualType MutTy = Args[M]->getType().getNonReferenceType();
    if (MutTy->isPointerType())
      MutTy = MutTy->getPointeeType();
    const CXXRecordDecl *MutatedRecord = MutTy->getAsCXXRecordDecl();
    CurrentBlockFacts.push_back(FactMgr.createFact<ArgOverlapFact>(
        Call, FactMgr.copyToFactStorage(llvm::ArrayRef<OriginID>(MutOrigins)),
        MutatedRecord,
        FactMgr.copyToFactStorage(llvm::ArrayRef<OriginID>(Borrows))));
  }
}

void FactsGenerator::handleImplicitObjectFieldUses(const Expr *Call,
                                                   const FunctionDecl *FD) {
  const auto *MemberCall = dyn_cast_or_null<CXXMemberCallExpr>(Call);
  if (!MemberCall)
    return;

  if (!isThisExpr(MemberCall->getImplicitObjectArgument()))
    return;

  const auto *MD = dyn_cast<CXXMethodDecl>(FD);
  assert(MD && "Function must be a CXXMethodDecl for member calls");

  const auto *ClassDecl = MD->getParent()->getDefinition();
  if (!ClassDecl)
    return;

  const auto UseFields = [&](const CXXRecordDecl *RD) {
    for (const auto *Field : RD->fields())
      if (FactMgr.getOriginMgr().isAccessedField(Field))
        if (auto *FieldNode = getOriginNode(*Field))
          CurrentBlockFacts.push_back(
              FactMgr.createFact<UseFact>(Call, FieldNode));
  };

  UseFields(ClassDecl);

  ClassDecl->forallBases([&](const CXXRecordDecl *Base) {
    UseFields(Base);
    return true;
  });
}

void FactsGenerator::handleLifetimeCaptureBy(const FunctionDecl *FD,
                                             ArrayRef<const Expr *> Args) {
  if (Args.empty())
    return;
  // FIXME: Add support for capture_by on constructors.
  if (isa<CXXConstructorDecl>(FD))
    return;
  const auto *Method = dyn_cast<CXXMethodDecl>(FD);
  bool IsInstance =
      Method && Method->isInstance() && !isa<CXXConstructorDecl>(FD);
  auto getArgCaptureBy = [FD, IsInstance](unsigned I) -> LifetimeCaptureByAttr * {
    // FIXME: Add support for capture_by on the implicit object (I == 0).
    const ParmVarDecl *PVD = paramForArg(FD, IsInstance, I);
    return PVD ? PVD->getAttr<LifetimeCaptureByAttr>() : nullptr;
  };
  for (unsigned I = 0; I < Args.size(); ++I) {
    const LifetimeCaptureByAttr *Attr = getArgCaptureBy(I);
    if (!Attr)
      continue;
    OriginNode *CapturedOriginNode = getOriginNode(*Args[I]);
    if (!CapturedOriginNode)
      continue;
    if (!CapturedOriginNode)
      continue;
    for (int CapturingArgIdx : Attr->params()) {
      // FIXME: Add support for capturing to Global/unknown.
      if (CapturingArgIdx == LifetimeCaptureByAttr::Global ||
          CapturingArgIdx == LifetimeCaptureByAttr::Unknown ||
          CapturingArgIdx == LifetimeCaptureByAttr::Invalid)
        continue;
      // CapturingArgIdx indexes the attribute's entity numbering, which matches
      // the modeled argument list directly: for an instance method the implicit
      // object (`this`, ArgIndex::This == 0) is Args[0] and each parameter maps
      // to its own Args slot; for a free or explicit-object function Args[0] is
      // the first parameter (index 0). So index Args directly. (Dropping the
      // object and reusing the index double-counted `this`: it mis-targeted the
      // capturer and ran one past the end -- crashing -- when the named capturer
      // was the last parameter of a non-static member function.)
      if (CapturingArgIdx < 0 || (size_t)CapturingArgIdx >= Args.size())
        continue;
      const Expr *CapturedByArg = Args[CapturingArgIdx];
      assert(CapturedByArg && "Capturer expression must be valid");

      OriginNode *CapturingOriginNode = getOriginNode(*CapturedByArg);
      OriginNode *Dest = getRValueOrigins(CapturedByArg, CapturingOriginNode);
      if (!Dest)
        continue;
      // KillDest=false because we cannot know if previous captures are being
      // replaced or accumulated. Multiple successive captures into the same
      // destination must all be tracked, so captured lifetimes are always
      // merged.
      CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
          Dest->getOriginID(), CapturedOriginNode->getOriginID(),
          /*KillDest=*/false));

      // Soundness: capturing the argument into the receiver object (`this`) is a
      // store into that object. If the argument borrows a member of the
      // receiver, the object becomes self-referential -- the same hazard as
      // `this->view = this->member;`. Emit a FieldStore so the checker's
      // (loan-based) self-referential detection sees it, using the most-derived
      // receiver (strip the implicit derived-to-base cast on the implicit object
      // argument) so a capture into a base-subobject view is keyed on the
      // derived class's fields too.
      if (CapturingArgIdx == LifetimeCaptureByAttr::This) {
        if (OriginNode *Recv = getOriginNode(*Args[0]->IgnoreImpCasts()))
          CurrentBlockFacts.push_back(FactMgr.createFact<FieldStoreFact>(
              Args[I], CapturedOriginNode->getOriginID(), Recv->getOriginID()));
        // The capture is an escape *out of the analyzed function* only when the
        // receiver is this function's own implicit object (`this`): then the
        // captured borrow becomes reachable from the caller's object. Capturing
        // into some other receiver (e.g. `localObj.capture(arg)`, where `Dest`
        // is a local's origin) stays within the function and is not an escape.
        // Record the escape so the annotation checker sees it -- a
        // [[clang::noescape]] parameter forwarded here violates its promise,
        // which is otherwise missed because the capture flows into the
        // whole-object `this` origin and never reaches a return/field/global
        // escape point.
        if (std::optional<OriginNode *> ThisOrigins =
                FactMgr.getOriginMgr().getThisOrigins();
            ThisOrigins &&
            Dest->getOriginID() == (*ThisOrigins)->getOriginID())
          CurrentBlockFacts.push_back(
              FactMgr.createFact<CapturedByThisEscapeFact>(
                  CapturedOriginNode->getOriginID(), Args[I]));
      }
    }
  }
}

// Comparison and relational operators (==, !=, <, >, <=, >=, <=>) compare their
// operands and yield a bool/ordering; by convention they neither capture an
// argument nor return a borrow into one, so passing an origin-carrying value
// (e.g. an iterator or view) to them is safe and needs no annotation. This is
// what makes a range-based for loop's `__begin != __end` test clean.
static bool isComparisonOperator(const FunctionDecl *FD) {
  switch (FD->getOverloadedOperator()) {
  case OO_EqualEqual:
  case OO_ExclaimEqual:
  case OO_Less:
  case OO_Greater:
  case OO_LessEqual:
  case OO_GreaterEqual:
  case OO_Spaceship:
    return true;
  default:
    return false;
  }
}

// Soundness: flag arguments bound to origin-carrying parameters (raw
// pointer/reference, gsl::Pointer, etc.) that carry no lifetime annotation and
// are not modeled via GSL recognition. The analysis cannot tell whether such a
// borrow escapes the call. Runs independently of the call's return type.
void FactsGenerator::handleUnannotatedIndirectionArgs(
    const FunctionDecl *FD, ArrayRef<const Expr *> Args) {
  const auto *Method = dyn_cast<CXXMethodDecl>(FD);
  bool IsInstance =
      Method && Method->isInstance() && !isa<CXXConstructorDecl>(FD);
  // Comparison/relational operators do not capture their operands.
  if (isComparisonOperator(FD))
    return;
  // A copy/move constructor or copy/move assignment copies or moves its source
  // rather than capturing the reference to it; any borrows the value itself
  // carries are propagated by the origin model (see VisitCXXConstructExpr), so
  // the source reference parameter is effectively noescape. This also avoids
  // false positives on the implicit special members synthesized for passing a
  // value-type argument by value, which cannot be annotated.
  if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(FD);
      Ctor && Ctor->isCopyOrMoveConstructor())
    return;
  if (Method &&
      (Method->isCopyAssignmentOperator() || Method->isMoveAssignmentOperator()))
    return;
  for (unsigned I = 0; I < Args.size(); ++I) {
    // The object argument (I == 0 for an instance method, implicit or C++23
    // explicit) is the receiver, not a borrow parameter to annotate; skip it.
    if (IsInstance && I == 0)
      continue;
    // Map the argument index to its declared parameter.
    const ParmVarDecl *PVD = paramForArg(FD, IsInstance, I);
    if (!PVD) {
      // An argument passed through the C variadic ellipsis (`...`) has no
      // declared parameter, so it cannot carry a lifetime annotation and the
      // analysis cannot model where the callee stores it. A borrow passed this
      // way escapes silently; reject it like an unannotated indirection
      // parameter.
      if (FD->isVariadic() && I >= FD->getNumParams() + IsInstance &&
          hasOrigins(Args[I]->getType()))
        CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
            UntrackedConstructReason::UnannotatedIndirection, Args[I]));
      continue;
    }
    if (!hasOrigins(PVD->getType()))
      continue;
    if (PVD->hasAttr<clang::LifetimeBoundAttr>() ||
        PVD->hasAttr<clang::NoEscapeAttr>() ||
        PVD->hasAttr<clang::LifetimeCaptureByAttr>())
      continue;
    // Heuristic: a parameter that the callee copies/moves in (rather than
    // capturing as a borrow) is effectively noescape. The value is copied in
    // only when the level of storage it refers to cannot itself hold a borrow.
    //
    //  * STL container insertion methods (push_back, insert, emplace, ...) take
    //    the element by const/rvalue *reference* and copy it in -- safe only
    //    when the referent is not pointer-like (so 'vector<int*>::push_back(&x)'
    //    is still surfaced, and 'emplace_back' building a view from a pointer
    //    stays flagged).
    //  * STL container constructors copy in the values they are given. Here a
    //    *pointer* parameter is also a copy-in when its pointee cannot hold a
    //    borrow -- this is what makes 'std::string s = "abc"' (the
    //    'basic_string(const char *)' constructor) clean: the characters are
    //    copied, the pointer does not escape.
    //  * std::string concatenation operators ('s += x', 'a + b') deep-copy their
    //    operands' characters; a string reference or a character pointer operand
    //    does not escape. Unlike the others this also applies to the free
    //    'operator+', which is not a method.
    //
    // It never applies to arbitrary user constructors, which may capture.
    {
      QualType ParamTy = PVD->getType();
      bool IsContainerCtor = Method && isa<CXXConstructorDecl>(Method) &&
                             isStlContainerType(Method->getParent());
      bool IsCopyInByRef =
          (Method && isStlContainerInsertionMethod(*Method)) ||
          IsContainerCtor || isStlStringConcatenationOperator(*FD);
      bool IsCopyInByPtr = IsContainerCtor || isStlStringConcatenationOperator(*FD);
      if (ParamTy->isReferenceType() &&
          !hasOrigins(ParamTy.getNonReferenceType()) && IsCopyInByRef)
        continue;
      if (ParamTy->isPointerType() && !hasOrigins(ParamTy->getPointeeType()) &&
          IsCopyInByPtr)
        continue;
    }
    // std::basic_string owns its buffer and never retains a borrow from an
    // argument: it copies (append/assign/operator=/insert/replace/ctor) or only
    // reads (compare/find/...) the characters of a string-source argument. So a
    // std::string_view / const char* / std::string argument does not escape,
    // even though a string_view itself can hold a borrow (unlike the copy-in
    // checks above, this holds precisely because the *referent* is a string the
    // owner copies, not a value it stores).
    if (isStlStringMemberCall(FD) && isStringSourceType(PVD->getType()))
      continue;
    // Skip arguments the analysis already models through GSL recognition.
    if ((I == 0 && shouldTrackFirstArgument(FD)) ||
        (I == 1 && shouldTrackSecondArgument(FD)))
      continue;
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::UnannotatedIndirection, Args[I]));
  }
}


void FactsGenerator::handleLambdaCallInvalidation(const Expr *Call,
                                                  const FunctionDecl *FD,
                                                  ArrayRef<const Expr *> Args) {
  // A call may invoke a callable it receives -- for `closure()` the closure is
  // the receiver (argument 0 of operator()); for `run(closure)` it is a passed
  // argument that the callee may invoke. Invoking a callable can mutate whatever
  // it holds non-const access to: the variables it captured by reference (the
  // reference is not const even though operator() is). A
  // [[clang::noescape]] parameter does not prevent that -- it only promises the
  // borrow is not stored beyond the call.
  //
  // The callable's value already carries the loans of its by-reference captures:
  // VisitLambdaExpr flows each capture's origin into the closure, and
  // std::function construction/assignment flows the stored closure's origins
  // onward. So rather than match the lambda literal syntactically (which misses
  // a closure laundered through a std::function variable, a `?:`, a wrapper,
  // ...), invalidate the loans the callable argument's value origin holds and
  // let the checker connect them to any live borrow. The invocation may happen
  // in a callee we cannot see, so this is done at the call site, where a borrow
  // into the captured owner may be live.
  (void)FD;
  for (const Expr *Arg : Args) {
    const CXXRecordDecl *RD =
        Arg->getType().getNonReferenceType()->getAsCXXRecordDecl();
    if (!RD || !(RD->isLambda() || isStdCallableWrapperType(RD)))
      continue;
    if (OriginNode *N = getRValueOrigins(Arg, getOriginNode(*Arg)))
      CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
          N->getOriginID(), Call, /*Assumed=*/true));
    // If the invoked callable is a data member, invoking it can mutate the
    // enclosing object: the callable may have captured that object (or its
    // members) by reference in a *different* function -- e.g. a [this]-capturing
    // closure stored into a std::function member by the constructor -- so those
    // captured loans are not visible on the member's own origin here. The
    // enclosing object's origin carries its loan (e.g. `$this`), so invalidate
    // it too. This catches a live borrow into the object (e.g. from a
    // lifetimebound accessor) across the invocation -- including from a *const*
    // method, where a stored callable subverts the "const does not mutate"
    // assumption that assumed-invalidation of non-const calls relies on.
    if (const auto *ME = dyn_cast<MemberExpr>(Arg->IgnoreImpCasts()))
      if (OriginNode *Base = getOriginNode(*ME->getBase()))
        CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
            Base->getOriginID(), Call, /*Assumed=*/true));
  }
}

// Soundness: an ownership-transferring move of an owner (std::move/forward of
// a gsl::Owner, or std::unique_ptr::release) is not modeled, so it silences the
// analysis for the moved-from object. Moving a pointer-like value is a harmless
// copy and is not flagged.
void FactsGenerator::handleMoveSilencing(const Expr *Call,
                                         const FunctionDecl *FD,
                                         ArrayRef<const Expr *> Args) {
  if (isStdReferenceCast(FD) && !Args.empty() &&
      isGslOwnerType(Args[0]->getType())) {
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::MoveSilencing, Call));
    return;
  }
  if (const auto *MD = dyn_cast<CXXMethodDecl>(FD); MD && isUniquePtrRelease(*MD))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::MoveSilencing, Call));
}

void FactsGenerator::handleFunctionCall(const Expr *Call,
                                        const FunctionDecl *FD,
                                        ArrayRef<const Expr *> Args,
                                        bool IsGslConstruction) {
  OriginNode *CallNode = getOriginNode(*Call);
  // Ignore functions returning values with no origin.
  FD = getDeclWithMergedLifetimeBoundAttrs(FD);
  if (!FD) {
    // The callee could not be resolved to a function (e.g. a call through a
    // function or member-function pointer). Such callees cannot carry lifetime
    // annotations, so the call is not modeled; surface it for soundness.
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::IndirectCall, Call));
    return;
  }
  // All arguments to a function are a use of the corresponding expressions.
  for (const Expr *Arg : Args)
    handleUse(Arg);
  // Soundness: a call returning a user-defined type of unknown ownership
  // (it can hold a borrow but is annotated neither [[gsl::Owner]] nor
  // [[gsl::Pointer]]) produces a value the analysis cannot track. Constructor
  // calls are skipped -- constructing such a value is reported at its
  // declaration or surrounding context.
  bool IsPointerResult = false;
  if (!isa<CXXConstructorDecl>(FD) &&
      isUnknownOwnershipType(Call->getType(),
                             FactMgr.getUnknownOwnershipCache()))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::UnknownOwnership, Call));
  // Likewise a call returning a gsl::Owner container of indirections
  // (e.g. std::vector<int*>); per-element borrows are not tracked.
  else if (!isa<CXXConstructorDecl>(FD) &&
           isGslOwnerOfIndirection(Call->getType(),
                                   FactMgr.getUnknownOwnershipCache()))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::OwnerOfIndirection, Call));
  // Likewise a call returning a gsl::Pointer view of indirections
  // (e.g. std::span<int*>); the inner pointees are not tracked.
  else if (!isa<CXXConstructorDecl>(FD) &&
           isGslPointerOfIndirection(Call->getType(),
                                     FactMgr.getUnknownOwnershipCache()))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::PointerOfIndirection, Call));
  // Or a call returning a non-owner aggregate (std::pair/std::tuple) burying an
  // owner-/pointer-of-indirection in its template arguments; search them as for
  // a local declaration, reporting the precise buried element/pointee type.
  else if (QualType Nested =
               isa<CXXConstructorDecl>(FD)
                   ? QualType()
                   : findNestedOwnerOrPointerOfIndirection(
                         Call->getType(), FactMgr.getUnknownOwnershipCache(),
                         IsPointerResult);
           !Nested.isNull())
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        IsPointerResult ? UntrackedConstructReason::PointerOfIndirection
                        : UntrackedConstructReason::OwnerOfIndirection,
        Call, Nested));
  handleInvalidatingCall(Call, FD, Args);
  handleAssumedInvalidatingCall(Call, FD, Args);
  handleLambdaCallInvalidation(Call, FD, Args);
  handleDestructiveCall(Call, FD, Args);
  handleArgumentOverlap(Call, FD, Args);
  handleMovedArgsInCall(FD, Args);
  handleImplicitObjectFieldUses(Call, FD);
  handleLifetimeCaptureBy(FD, Args);
  handleUnannotatedIndirectionArgs(FD, Args);
  handleMoveSilencing(Call, FD, Args);
  if (!CallNode)
    return;
  // A [[clang::lifetime_immortal]] function returns storage that lives for the
  // whole program. Model it as a borrow of immortal storage that never expires,
  // independent of the arguments.
  if (FD->hasAttr<clang::LifetimeImmortalAttr>()) {
    const Loan *L =
        FactMgr.getLoanMgr().createLoan(AccessPath::Immortal(FD), Call);
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), CallNode->getOriginID()));
    return;
  }
  // A function with __attribute__((malloc)) returns a fresh heap allocation.
  // Model it like `new` so the result is tracked and a later free/delete of it
  // is not reported as a naked deallocation.
  if (FD->hasAttr<clang::RestrictAttr>()) {
    const Loan *L = FactMgr.getLoanMgr().createLoan(
        AccessPath::HeapAllocation(Call), Call);
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), CallNode->getOriginID()));
    return;
  }
  if (isStdReferenceCast(FD)) {
    assert(Args.size() == 1 &&
           "std reference cast builtins take exactly one argument");
    // std reference-cast functions like std::move return a result that refers
    // to the same object as the argument, so propagate the full origins.
    flow(CallNode, getOriginNode(*Args[0]), /*Kill=*/true);
    return;
  }
  auto IsArgLifetimeBound = [FD, &Args](unsigned I) -> bool {
    const ParmVarDecl *PVD = nullptr;
    if (const auto *Method = dyn_cast<CXXMethodDecl>(FD);
        Method && Method->isInstance() && !isa<CXXConstructorDecl>(FD)) {
      if (Method->isExplicitObjectMemberFunction()) {
        // C++23 deducing-this: the explicit object parameter `self` is
        // getParamDecl(0), and the argument list (object first, then the
        // explicit arguments) maps 1:1 to the parameters. So Args[I]'s
        // lifetimebound-ness -- including the object argument at I == 0 -- comes
        // from getParamDecl(I), not the method itself.
        if (I < Method->getNumParams())
          PVD = Method->getParamDecl(I);
      } else if (I == 0) {
        // For the implicit 'this' argument, the attribute is on the method.
        return implicitObjectParamIsLifetimeBound(Method) ||
               shouldTrackImplicitObjectArg(
                   *Args[0], Method, /*RunningUnderLifetimeSafety=*/true);
      } else if ((I - 1) < Method->getNumParams()) {
        // For explicit arguments, find the corresponding parameter declaration.
        PVD = Method->getParamDecl(I - 1);
      }
    } else if (I == 0 && shouldTrackFirstArgument(FD)) {
      return true;
    } else if (I == 1 && shouldTrackSecondArgument(FD)) {
      return true;
    } else if (I < FD->getNumParams()) {
      // For free functions or static methods.
      PVD = FD->getParamDecl(I);
    }
    return PVD ? PVD->hasAttr<clang::LifetimeBoundAttr>() : false;
  };
  auto shouldTrackPointerImplicitObjectArg = [FD, &Args](unsigned I) -> bool {
    const auto *Method = dyn_cast<CXXMethodDecl>(FD);
    if (!Method || !Method->isInstance())
      return false;
    return I == 0 &&
           isGslPointerType(Method->getFunctionObjectParameterType()) &&
           shouldTrackImplicitObjectArg(*Args[0], Method,
                                        /*RunningUnderLifetimeSafety=*/true);
  };
  if (Args.empty()) {
    issueUnknownLoanIfUntrackedBorrow(Call, CallNode, /*FlowedIntoResult=*/false);
    return;
  }
  bool KillSrc = true;
  for (unsigned I = 0; I < Args.size(); ++I) {
    OriginNode *ArgNode = getOriginNode(*Args[I]);
    if (!ArgNode)
      continue;
    if (IsGslConstruction) {
      // TODO: document with code example.
      // std::string_view(const std::string_view& from)
      if (isGslPointerType(Args[I]->getType())) {
        assert(!Args[I]->isGLValue() || ArgNode->getLength() >= 2);
        ArgNode = getRValueOrigins(Args[I], ArgNode);
      }
      if (isGslOwnerType(Args[I]->getType())) {
        // The constructed gsl::Pointer borrows from the Owner's storage, not
        // from what the Owner itself borrows, so only the outermost origin is
        // needed.
        //
        // Soundness: a view borrowing a mutable global/static owner -- whether
        // the owner itself (`g_str`) or its contents (`g_table[i]`) -- can be
        // invalidated by mutating that owner from anywhere the intra-procedural
        // analysis cannot see. That is detected loan-based in the checker
        // (LifetimeChecker::flagBorrowFromMutableGlobal): the borrow surfaces as
        // a loan rooted at the global owner regardless of how it was reached.
        CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
            CallNode->getOriginID(), ArgNode->getOriginID(), KillSrc));
        KillSrc = false;
      } else if (IsArgLifetimeBound(I)) {
        // Only flow the outer origin here. For lifetimebound args in
        // gsl::Pointer construction, we do not have enough information to
        // safely match inner origins, so the source and
        // destination origin lists may have different lengths.
        // FIXME: Handle origin-shape mismatches gracefully so we can also flow
        // inner origins.
        flowSingleLevelWithUnknownDepth(CallNode, ArgNode, Call, KillSrc);
        KillSrc = false;
      }
    } else if (shouldTrackPointerImplicitObjectArg(I)) {
      assert(ArgNode->getLength() >= 2 &&
             "Object arg of pointer type should have at least two origins");
      // See through the GSLPointer reference to see the pointer's value.
      CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
          CallNode->getOriginID(), ArgNode->getPointeeChild()->getOriginID(),
          KillSrc));
      KillSrc = false;
    } else if (IsArgLifetimeBound(I)) {
      // Lifetimebound on a non-GSL-ctor function means the returned
      // pointer/reference itself must not outlive the arguments. This
      // only constrains the top-level origin. If the return is itself a
      // multi-level indirection (e.g. a `const View&` -- a reference to a view,
      // as std::max/min/clamp return), the deeper levels are seeded with an
      // Unknown loan so the view's own (inner) borrow is reported as lost rather
      // than silently dropped (which a control-flow merge could otherwise mask).
      flowSingleLevelWithUnknownDepth(CallNode, ArgNode, Call, KillSrc);
      KillSrc = false;
    }
  }
  // If the call returns a borrow-carrying value but no loan was propagated into
  // it (KillSrc still true), the borrow is untracked -- e.g. a view-returning
  // method that is not [[clang::lifetimebound]] and not a recognized accessor,
  // such as std::string_view::substr. Mark the result with an Unknown loan so
  // the lost borrow is reported when the result is used, robustly across
  // dataflow joins (an empty loan set would be masked by a co-resident valid
  // borrow on another path).
  issueUnknownLoanIfUntrackedBorrow(Call, CallNode, /*FlowedIntoResult=*/!KillSrc);
}

void FactsGenerator::issueUnknownLoanIfUntrackedBorrow(const Expr *Call,
                                                       OriginNode *CallNode,
                                                       bool FlowedIntoResult) {
  if (!CallNode || FlowedIntoResult)
    return;
  // Only a value that itself carries a borrow at its top level: a view
  // (gsl::Pointer) or a raw pointer/reference. An owner is tracked differently
  // and a non-pointer return holds no borrow.
  //
  // A reference-returning call (e.g. an unrecognized `T& f()` such as
  // std::vector::emplace_back) is a glvalue: the AST strips the reference, so
  // Call->getType() is the pointee type and `isReferenceType()` never holds for
  // a call. Detect the reference return via the value category instead --
  // otherwise `&f()` borrows storage the analysis cannot track yet the result
  // origin stays *empty* (not sentineled), and a control-flow merge supplying a
  // valid loan on another path would mask the loss (the Unknown loan, by
  // contrast, survives the union join).
  QualType RetTy = Call->getType();
  if (!isGslPointerType(RetTy) && !RetTy->isPointerType() &&
      !RetTy->isReferenceType() && !Call->isGLValue())
    return;
  const Loan *L =
      FactMgr.getLoanMgr().createLoan(AccessPath::Unknown(Call), Call);
  CurrentBlockFacts.push_back(
      FactMgr.createFact<IssueFact>(L->getID(), CallNode->getOriginID()));
}

/// Checks if the expression is a `void("__lifetime_test_point_...")` cast.
/// If so, creates a `TestPointFact` and returns true.
bool FactsGenerator::handleTestPoint(const CXXFunctionalCastExpr *FCE) {
  if (!FCE->getType()->isVoidType())
    return false;

  const auto *SubExpr = FCE->getSubExpr()->IgnoreParenImpCasts();
  if (const auto *SL = dyn_cast<StringLiteral>(SubExpr)) {
    llvm::StringRef LiteralValue = SL->getString();
    const std::string Prefix = "__lifetime_test_point_";

    if (LiteralValue.starts_with(Prefix)) {
      StringRef Annotation = LiteralValue.drop_front(Prefix.length());
      CurrentBlockFacts.push_back(
          FactMgr.createFact<TestPointFact>(Annotation));
      return true;
    }
  }
  return false;
}

void FactsGenerator::handleUse(const Expr *E) {
  OriginNode *Node = getOriginNode(*E);
  if (!Node)
    return;
  // For DeclRefExpr: Remove the outer layer of origin which borrows from the
  // decl directly (e.g., when this is not a reference). This is a use of the
  // underlying decl.
  if (auto *DRE = dyn_cast<DeclRefExpr>(E);
      DRE && !DRE->getDecl()->getType()->isReferenceType())
    Node = getRValueOrigins(DRE, Node);
  // Skip if there is no inner origin (e.g., when it is not a pointer type).
  if (!Node)
    return;
  if (!UseFacts.contains(E)) {
    UseFact *UF = FactMgr.createFact<UseFact>(E, Node);
    CurrentBlockFacts.push_back(UF);
    UseFacts[E] = UF;
  }
}

// Creates an IssueFact for a new placeholder loan for each pointer or reference
// parameter at the function's entry.
llvm::SmallVector<Fact *> FactsGenerator::issuePlaceholderLoans() {
  const auto *FD = dyn_cast<FunctionDecl>(AC.getDecl());
  if (!FD)
    return {};

  llvm::SmallVector<Fact *> PlaceholderLoanFacts;
  if (auto ThisOrigins = FactMgr.getOriginMgr().getThisOrigins()) {
    OriginNode *Node = *ThisOrigins;
    const Loan *L = FactMgr.getLoanMgr().createLoan(
        AccessPath::Placeholder(cast<CXXMethodDecl>(FD)),
        /*IssuingExpr=*/nullptr);
    PlaceholderLoanFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), Node->getOriginID()));

    // Seed the implicit object's pointer/view *members* with a non-expiring
    // "uninitialized" loan, recursively through by-value sub-objects and array
    // elements (which share one element origin). The members are caller-provided
    // storage, so reading one that this function never wrote is not a lost
    // borrow; a borrow stored into a member later kills and replaces the seed.
    // Mirrors the local-array seeding in VisitDeclStmt, but for `this`.
    //
    // A member read resolves to the origin reached from the field's own origin
    // node (`this->f` uses getOriginNode(f); `this->f.g` narrows into that
    // node's `g` subtree), so seed by walking the origin tree of each field of
    // the implicit object's record (and its bases).
    const auto *MD = cast<CXXMethodDecl>(FD);
    llvm::SmallPtrSet<const OriginNode *, 16> SeededNodes;
    auto seedTree = [&](auto &&Self, OriginNode *N, const FieldDecl *Tag) -> void {
      if (!N || !SeededNodes.insert(N).second)
        return;
      if (const Type *Ty =
              FactMgr.getOriginMgr().getOrigin(N->getOriginID()).Ty) {
        QualType QT(Ty, 0);
        if (isPointerLikeType(QT) || QT->isReferenceType()) {
          const Loan *ML = FactMgr.getLoanMgr().createLoan(
              AccessPath::Uninitialized(Tag), /*IssuingExpr=*/nullptr);
          PlaceholderLoanFacts.push_back(FactMgr.createFact<IssueFact>(
              ML->getID(), N->getOriginID()));
        }
      }
      for (const OriginNode::Edge &E : N->children())
        Self(Self, E.Child, Tag);
    };
    llvm::SmallPtrSet<const CXXRecordDecl *, 8> VisitedRD;
    auto seedRecord = [&](auto &&Self, const CXXRecordDecl *RD) -> void {
      if (!RD || !RD->hasDefinition() || !VisitedRD.insert(RD).second)
        return;
      for (const CXXBaseSpecifier &B : RD->bases())
        Self(Self, B.getType()->getAsCXXRecordDecl());
      for (const FieldDecl *F : RD->fields()) {
        // Owners are opaque leaves (a borrow into an owner field is tracked via
        // its own field-rooted loan, not seeded here); skip them.
        if (isMutableOwnerType(F->getType()) ||
            !FactMgr.getOriginMgr().hasOrigins(F->getType()))
          continue;
        if (OriginNode *FN = getOriginNode(*F))
          seedTree(seedTree, FN, F);
      }
    };
    seedRecord(seedRecord, MD->getParent());
  }
  for (const ParmVarDecl *PVD : FD->parameters()) {
    OriginNode *Node = getOriginNode(*PVD);
    if (!Node)
      continue;
    const Loan *L = FactMgr.getLoanMgr().createLoan(
        AccessPath::Placeholder(PVD), /*IssuingExpr=*/nullptr);
    PlaceholderLoanFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), Node->getOriginID()));
  }
  return PlaceholderLoanFacts;
}

} // namespace clang::lifetimes::internal
