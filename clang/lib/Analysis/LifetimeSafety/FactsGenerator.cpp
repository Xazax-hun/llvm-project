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
#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/Analysis/Analyses/LifetimeSafety/FactsGenerator.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Origins.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/CFG.h"
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

/// Returns true if `QT` is a mutable [[gsl::Owner]] (peeling arrays): a borrow
/// of such a field can be invalidated by reallocating it. A `const` owner can
/// never be reallocated, so it is excluded.
static bool isMutableOwnerType(QualType QT) {
  QT = QT.getNonReferenceType();
  while (QT->isArrayType())
    QT = QT->getAsArrayTypeUnsafe()->getElementType();
  return isGslOwnerType(QT) && !QT.isConstQualified();
}

/// Returns true if `RD` has a reachable mutable owner data member -- directly,
/// transitively (a field whose own record contains one), or through a base
/// class. A non-const member call on such an object may reallocate that owner,
/// invalidating a view into it. `Visited` cuts cycles.
static bool recordContainsMutableOwner(
    const CXXRecordDecl *RD, llvm::SmallPtrSet<const CXXRecordDecl *, 8> &Visited) {
  if (!RD || !RD->hasDefinition())
    return false;
  if (!Visited.insert(RD->getCanonicalDecl()).second)
    return false;
  for (const CXXBaseSpecifier &B : RD->bases())
    if (recordContainsMutableOwner(B.getType()->getAsCXXRecordDecl(), Visited))
      return true;
  for (const FieldDecl *FD : RD->fields()) {
    if (isMutableOwnerType(FD->getType()))
      return true;
    // Recurse into a non-owner record field (e.g. an aggregate sub-object that
    // itself holds an owner). Owners are leaves -- we never descend into them.
    QualType FT = FD->getType().getNonReferenceType();
    while (FT->isArrayType())
      FT = FT->getAsArrayTypeUnsafe()->getElementType();
    if (!isGslOwnerType(FT) &&
        recordContainsMutableOwner(FT->getAsCXXRecordDecl(), Visited))
      return true;
  }
  return false;
}

/// Used by the safe model to treat a non-const member call on an object as
/// invalidating views into its (possibly transitive / inherited) owner fields.
static bool recordHasGslOwnerField(QualType QT) {
  QT = QT.getNonReferenceType();
  // The implicit object argument of a member call is the `this` pointer
  // (type `S*`); peel it to reach the record.
  if (QT->isPointerType())
    QT = QT->getPointeeType();
  llvm::SmallPtrSet<const CXXRecordDecl *, 8> Visited;
  return recordContainsMutableOwner(QT->getAsCXXRecordDecl(), Visited);
}

/// Returns true if a parameter of type `PT` lets the call mutate the owner the
/// argument refers to: a non-const pointer/reference to an owner (or to a record
/// that transitively contains a mutable owner field), or a gsl::Pointer that
/// exposes mutable access to a non-const owner pointee. Shared by the assumed-
/// invalidation and argument-overlap checks.
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
  if (isGslPointerType(PT.getNonReferenceType()))
    return pointsToMutableOwner(PT.getNonReferenceType());
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
  if (!isInStlNamespace(MD.getParent()))
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
  llvm::SmallVector<Fact *> PlaceholderLoanFacts = issuePlaceholderLoans();
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

void FactsGenerator::VisitDeclStmt(const DeclStmt *DS) {
  for (const Decl *D : DS->decls()) {
    const auto *VD = dyn_cast<VarDecl>(D);
    if (!VD)
      continue;
    // Soundness: a local of a user-defined type whose ownership is unknown.
    if (isUnknownOwnershipType(VD->getType(),
                               FactMgr.getUnknownOwnershipCache()))
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::UnknownOwnership, VD));
    // Soundness: a local gsl::Owner container whose elements are indirections
    // (e.g. std::vector<int*>); per-element borrows are not tracked.
    else if (isGslOwnerOfIndirection(VD->getType()))
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::OwnerOfIndirection, VD));
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
  handleUse(DRE);
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
}

void FactsGenerator::VisitCXXDefaultInitExpr(const CXXDefaultInitExpr *DIE) {
  if (const Expr *Init = DIE->getExpr())
    killAndFlowOrigin(*DIE, *Init);
}

void FactsGenerator::handleCXXCtorInitializer(const CXXCtorInitializer *CII) {
  // Flows origins from the initializer expression to the field.
  // Example: `MyObj(std::string s) : view(s) {}`
  if (const FieldDecl *FD = CII->getAnyMember())
    killAndFlowOrigin(*FD, *CII->getInit());
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
    bool OwnerField = isMutableOwnerType(FD->getType());
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
}

void FactsGenerator::VisitCallExpr(const CallExpr *CE) {
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
  case CK_ArrayToPointerDecay:
    assert(Src && "Array expression should have origins as it is GL value");
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
    return;
  }
}

void FactsGenerator::VisitUnaryOperator(const UnaryOperator *UO) {
  switch (UO->getOpcode()) {
  case UO_AddrOf: {
    const Expr *SubExpr = UO->getSubExpr();
    // The origin of an address-of expression (e.g., &x) is the origin of
    // its sub-expression (x). This fact will cause the dataflow analysis
    // to propagate any loans held by the sub-expression's origin to the
    // origin of this UnaryOperator expression.
    killAndFlowOrigin(*UO, *SubExpr);
    return;
  }
  case UO_Deref: {
    const Expr *SubExpr = UO->getSubExpr();
    killAndFlowOrigin(*UO, *SubExpr);
    return;
  }
  default:
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
  // Assignment to an array element (`arr[i] = &x`). All elements share the
  // array's single element-origin, so we cannot tell which element is
  // overwritten: merge the new loans in rather than killing the old ones (the
  // origin conservatively holds the loans of every element ever stored).
  bool MergeIntoSharedElement = false;
  if (const auto *ASE_LHS = dyn_cast<ArraySubscriptExpr>(LHSExpr);
      ASE_LHS &&
      ASE_LHS->getBase()->IgnoreParenImpCasts()->getType()->isArrayType()) {
    LHSNode = getOriginNode(*ASE_LHS);
    MergeIntoSharedElement = LHSNode != nullptr;
  }

  if (!LHSNode)
    return;
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
  if (const auto *ME_LHS = dyn_cast<MemberExpr>(LHSExpr))
    if (const auto *LF = dyn_cast<FieldDecl>(ME_LHS->getMemberDecl());
        LF && (isGslPointerType(LF->getType()) ||
               LF->getType()->isPointerOrReferenceType())) {
      // Use the static type of the *receiver object* as the enclosing object,
      // not the type that declares the member. When the member lives in a base
      // class, `ME_LHS->getBase()` is the receiver implicitly cast to that base
      // (`(Base*)this`); stripping the implicit derived-to-base cast recovers
      // the most-derived receiver (`this` of type `Derived*`), so a view in a
      // base subobject bound to a member of the derived class is recognized as
      // self-referential too -- the membership walk then sees the derived
      // class's fields.
      const Expr *Base = ME_LHS->getBase()->IgnoreImpCasts();
      if (OriginNode *Container = getOriginNode(*Base))
        CurrentBlockFacts.push_back(FactMgr.createFact<FieldStoreFact>(
            ME_LHS, RHSNode->getOriginID(), Container->getOriginID()));
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
  if (BO->isCompoundAssignmentOp())
    return;
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
  //
  // TODO: Handle aggregate (record) list initialization.
  if (ILE->getNumInits() == 1 &&
      ILE->getType().getCanonicalType() ==
          ILE->getInit(0)->getType().getCanonicalType())
    killAndFlowOrigin(*ILE, *ILE->getInit(0));
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
  for (const Expr *Init : LE->capture_inits()) {
    if (!Init)
      continue;
    OriginNode *InitNode = getOriginNode(*Init);
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

void FactsGenerator::handlePlacementNew(const CXXNewExpr *NE,
                                        OriginNode *NewNode) {
  // Model only the standard single-argument placement new form, where the
  // placement argument corresponds to a void* allocation-function parameter.
  // Other placement forms, such as std::nothrow, are not modeled as providing
  // storage for the returned pointer.
  if (NE->getNumPlacementArgs() != 1)
    return;

  const FunctionDecl *OperatorNew = NE->getOperatorNew();
  if (OperatorNew->getNumParams() <= 1)
    return;

  const auto *Arg =
      OperatorNew->getParamDecl(1)->getType()->getAs<PointerType>();
  if (!Arg || !Arg->isVoidPointerType())
    return;

  // Use the placement argument before the implicit conversion to void*, so
  // inner origins are still available.
  const Expr *PlacementArg = NE->getPlacementArg(0);
  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(PlacementArg);
      ICE && ICE->getCastKind() == CK_BitCast &&
      PlacementArg->getType()->isVoidPointerType())
    PlacementArg = ICE->getSubExpr();
  OriginNode *PlacementNode = getOriginNode(*PlacementArg);
  // FIXME: General placement arguments need separate handling to overwrite
  // the right origins.

  // The pointer returned by placement new comes from the placement
  // argument.
  if (PlacementNode)
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        NewNode->getOriginID(), PlacementNode->getOriginID(), true));
}

void FactsGenerator::VisitCXXNewExpr(const CXXNewExpr *NE) {
  OriginNode *NewNode = getOriginNode(*NE);
  const Expr *Init = NE->getInitializer();

  if (NE->getNumPlacementArgs() == 1) {
    handlePlacementNew(NE, NewNode);
  } else {
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
    if (const auto *TS = dyn_cast<CXXTryStmt>(S))
      // A `try`/`catch` introduces exception-handling control flow that the
      // analysis does not model (the handler resumes after the stack has
      // unwound, with destructors having run). Surface it so a dangling borrow
      // along a handler path is not silently missed.
      CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
          UntrackedConstructReason::Exception, TS->getTryLoc()));
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
    bool OwnerReceiver = isGslOwnerType(Args[0]->getType());
    if ((OwnerReceiver || recordHasGslOwnerField(Args[0]->getType())) &&
        !(OwnerReceiver && isNonInvalidatingMethod(*Method)))
      if (OriginNode *L = getOriginNode(*Args[0]))
        CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
            L->getOriginID(), Call, /*Assumed=*/true));
  }
  // The implicit object argument (I == 0 for instance methods) is intentionally
  // skipped below -- it is handled by case (1) above.
  for (unsigned I = 0; I < Args.size(); ++I) {
    const ParmVarDecl *PVD = nullptr;
    if (IsInstance) {
      if (I == 0)
        continue; // Implicit object argument, handled above.
      if (I - 1 < Method->getNumParams())
        PVD = Method->getParamDecl(I - 1);
    } else if (I < FD->getNumParams()) {
      PVD = FD->getParamDecl(I);
    }
    if (!PVD)
      continue;
    if (!paramMayMutateOwner(PVD->getType()))
      continue;
    if (OriginNode *L = getOriginNode(*Args[I]))
      CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
          L->getOriginID(), Call, /*Assumed=*/true));
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

  // Whether the call may mutate the storage argument `I` refers to.
  auto IsMutatingArg = [&](unsigned I) -> bool {
    if (IsInstance && I == 0)
      // The receiver of any non-const instance method may be mutated. Whether
      // the method also returns a borrow (lifetimebound / a GSL accessor) is
      // orthogonal -- such a method can still reallocate the receiver -- so it
      // is not excluded here.
      return !Method->isConst();
    const ParmVarDecl *PVD = nullptr;
    if (IsInstance) {
      if (I - 1 < Method->getNumParams())
        PVD = Method->getParamDecl(I - 1);
    } else if (I < FD->getNumParams()) {
      PVD = FD->getParamDecl(I);
    }
    return PVD && paramMayMutateOwner(PVD->getType());
  };

  for (unsigned M = 0; M < Args.size(); ++M) {
    if (!IsMutatingArg(M))
      continue;
    OriginNode *MutNode = getOriginNode(*Args[M]);
    if (!MutNode)
      continue;
    // Collect the other borrow-holding arguments into a single fact for this
    // mutated owner (rather than one fact per pair).
    llvm::SmallVector<OriginID, 4> Borrows;
    for (unsigned B = 0; B < Args.size(); ++B) {
      if (B == M)
        continue;
      // Only a view/pointer co-argument can hold a borrow that aliases the
      // mutated owner's storage.
      QualType BT = Args[B]->getType().getNonReferenceType();
      if (!isGslPointerType(BT) && !BT->isPointerOrReferenceType())
        continue;
      if (OriginNode *BorrowNode =
              getRValueOrigins(Args[B], getOriginNode(*Args[B])))
        Borrows.push_back(BorrowNode->getOriginID());
    }
    if (Borrows.empty())
      continue;
    CurrentBlockFacts.push_back(FactMgr.createFact<ArgOverlapFact>(
        Call, MutNode->getOriginID(),
        FactMgr.copyToFactStorage(llvm::ArrayRef<OriginID>(Borrows))));
  }
}

void FactsGenerator::handleImplicitObjectFieldUses(const Expr *Call,
                                                   const FunctionDecl *FD) {
  const auto *MemberCall = dyn_cast_or_null<CXXMemberCallExpr>(Call);
  if (!MemberCall)
    return;

  if (!isa_and_present<CXXThisExpr>(
          MemberCall->getImplicitObjectArgument()->IgnoreImpCasts()))
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
  auto getArgCaptureBy = [FD,
                          IsInstance](unsigned I) -> LifetimeCaptureByAttr * {
    const ParmVarDecl *PVD = nullptr;
    if (IsInstance) {
      // FIXME: Add support for I == 0 i.e. capture_by on function declarations
      if (I > 0 && I - 1 < FD->getNumParams())
        PVD = FD->getParamDecl(I - 1);
    } else {
      if (I < FD->getNumParams())
        PVD = FD->getParamDecl(I);
    }
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
      ArrayRef<const Expr *> CallArgs = IsInstance ? Args.drop_front() : Args;
      const Expr *CapturedByArg =
          (CapturingArgIdx == LifetimeCaptureByAttr::This)
              ? Args[0]
              : CallArgs[CapturingArgIdx];
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
      if (CapturingArgIdx == LifetimeCaptureByAttr::This)
        if (OriginNode *Recv = getOriginNode(*Args[0]->IgnoreImpCasts()))
          CurrentBlockFacts.push_back(FactMgr.createFact<FieldStoreFact>(
              Args[I], CapturedOriginNode->getOriginID(), Recv->getOriginID()));
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
    // Map the argument index to its explicit parameter, skipping the implicit
    // object argument of instance methods.
    const ParmVarDecl *PVD = nullptr;
    if (IsInstance) {
      if (I == 0)
        continue;
      if (I - 1 < Method->getNumParams())
        PVD = Method->getParamDecl(I - 1);
    } else if (I < FD->getNumParams()) {
      PVD = FD->getParamDecl(I);
    }
    if (!PVD || !hasOrigins(PVD->getType()))
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
    // Skip arguments the analysis already models through GSL recognition.
    if ((I == 0 && shouldTrackFirstArgument(FD)) ||
        (I == 1 && shouldTrackSecondArgument(FD)))
      continue;
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::UnannotatedIndirection, Args[I]));
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
  if (!isa<CXXConstructorDecl>(FD) &&
      isUnknownOwnershipType(Call->getType(),
                             FactMgr.getUnknownOwnershipCache()))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::UnknownOwnership, Call));
  // Likewise a call returning a gsl::Owner container of indirections
  // (e.g. std::vector<int*>); per-element borrows are not tracked.
  else if (!isa<CXXConstructorDecl>(FD) &&
           isGslOwnerOfIndirection(Call->getType()))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::OwnerOfIndirection, Call));
  handleInvalidatingCall(Call, FD, Args);
  handleAssumedInvalidatingCall(Call, FD, Args);
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
      if (I == 0)
        // For the 'this' argument, the attribute is on the method itself.
        return implicitObjectParamIsLifetimeBound(Method) ||
               shouldTrackImplicitObjectArg(
                   *Args[0], Method, /*RunningUnderLifetimeSafety=*/true);
      if ((I - 1) < Method->getNumParams())
        // For explicit arguments, find the corresponding parameter
        // declaration.
        PVD = Method->getParamDecl(I - 1);
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
  if (Args.empty())
    return;
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
        // Soundness: a view borrowing a mutable global/static owner can be
        // invalidated by mutating that owner from anywhere (another function or
        // translation unit), which the intra-procedural analysis cannot see. A
        // const owner cannot be mutated, so it is safe.
        if (const auto *DRE =
                dyn_cast<DeclRefExpr>(Args[I]->IgnoreParenImpCasts()))
          if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
              VD && VD->hasGlobalStorage() &&
              !VD->getType().isConstQualified())
            CurrentBlockFacts.push_back(
                FactMgr.createFact<UntrackedConstructFact>(
                    UntrackedConstructReason::ViewOnMutableGlobal, Call));
        // The constructed gsl::Pointer borrows from the Owner's storage, not
        // from what the Owner itself borrows, so only the outermost origin is
        // needed.
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
        CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
            CallNode->getOriginID(), ArgNode->getOriginID(), KillSrc));
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
      // only constrains the top-level origin.
      CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
          CallNode->getOriginID(), ArgNode->getOriginID(), KillSrc));
      KillSrc = false;
    }
  }
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
