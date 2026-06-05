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

OriginList *FactsGenerator::getOriginsList(const ValueDecl &D) {
  return FactMgr.getOriginMgr().getOrCreateList(&D);
}
OriginList *FactsGenerator::getOriginsList(const Expr &E) {
  return FactMgr.getOriginMgr().getOrCreateList(&E);
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
/// This function enforces a critical type-safety invariant: both lists must
/// have the same shape (same depth/structure). This invariant ensures that
/// origins flow only between compatible types during expression evaluation.
///
/// Examples:
///   - `int* p = &x;` flows origins from `&x` (depth 1) to `p` (depth 1)
///   - `int** pp = &p;` flows origins from `&p` (depth 2) to `pp` (depth 2)
///     * Level 1: pp <- p's address
///     * Level 2: (*pp) <- what p points to (i.e., &x)
///   - `View v = obj;` flows origins from `obj` (depth 1) to `v` (depth 1)
void FactsGenerator::flow(OriginList *Dst, OriginList *Src, bool Kill) {
  if (!Dst)
    return;
  assert(Src &&
         "Dst is non-null but Src is null. List must have the same length");
  assert(Dst->getLength() == Src->getLength() &&
         "Lists must have the same length");

  while (Dst && Src) {
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        Dst->getOuterOriginID(), Src->getOuterOriginID(), Kill));
    Dst = Dst->peelOuterOrigin();
    Src = Src->peelOuterOrigin();
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
static OriginList *getRValueOrigins(const Expr *E, OriginList *List) {
  if (!List)
    return nullptr;
  return E->isGLValue() ? List->peelOuterOrigin() : List;
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
      if (OriginList *VDList = getOriginsList(*VD)) {
        const Loan *L = FactMgr.getLoanMgr().createLoan(
            AccessPath::Uninitialized(VD), /*IssuingExpr=*/nullptr);
        CurrentBlockFacts.push_back(
            FactMgr.createFact<IssueFact>(L->getID(), VDList->getOuterOriginID()));
      }
    if (const Expr *InitExpr = VD->getInit()) {
      OriginList *VDList = getOriginsList(*VD);
      if (!VDList)
        continue;
      OriginList *InitList = getOriginsList(*InitExpr);
      assert(InitList && "VarDecl had origins but InitExpr did not");
      flow(VDList, InitList, /*Kill=*/true);
    }
  }
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
    OriginList *List = getOriginsList(*DRE);
    assert(List &&
           "gl-value DRE of non-pointer type should have an origin list");
    // This loan specifically tracks borrowing the variable's storage location
    // itself and is issued to outermost origin (List->OID).
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), List->getOuterOriginID()));
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
    if (OriginList *ArgList = getRValueOrigins(Arg, getOriginsList(*Arg))) {
      flow(getOriginsList(*CCE), ArgList, /*Kill=*/true);
      return;
    }
  }
  // Standard library callable wrappers (e.g., std::function) propagate the
  // stored lambda's origins.
  if (const auto *RD = CCE->getType()->getAsCXXRecordDecl();
      RD && isStdCallableWrapperType(RD) && CCE->getNumArgs() == 1) {
    const Expr *Arg = CCE->getArg(0);
    if (OriginList *ArgList = getRValueOrigins(Arg, getOriginsList(*Arg))) {
      flow(getOriginsList(*CCE), ArgList, /*Kill=*/true);
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
  auto *MD = ME->getMemberDecl();
  if (isa<FieldDecl>(MD) && doesDeclHaveStorage(MD)) {
    assert(ME->isGLValue() && "Field member should be GL value");
    OriginList *Dst = getOriginsList(*ME);
    assert(Dst && "Field member should have an origin list as it is GL value");
    OriginList *Src = getOriginsList(*ME->getBase());
    assert(Src && "Base expression should be a pointer/reference type");
    // The field's glvalue (outermost origin) holds the same loans as the base
    // expression.
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        Dst->getOuterOriginID(), Src->getOuterOriginID(),
        /*Kill=*/true));
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
  getOriginsList(*N);
}

void FactsGenerator::VisitCastExpr(const CastExpr *CE) {
  OriginList *Dest = getOriginsList(*CE);
  if (!Dest)
    return;
  const Expr *SubExpr = CE->getSubExpr();
  OriginList *Src = getOriginsList(*SubExpr);

  switch (CE->getCastKind()) {
  case CK_LValueToRValue:
    if (!SubExpr->isGLValue())
      return;

    assert(Src && "LValue being cast to RValue has no origin list");
    // The result of an LValue-to-RValue cast on a pointer lvalue (like `q` in
    // `int *p, *q; p = q;`) should propagate the inner origin (what the pointer
    // points to), not the outer origin (the pointer's storage location). Strip
    // the outer lvalue origin.
    flow(getOriginsList(*CE), getRValueOrigins(SubExpr, Src),
         /*Kill=*/true);
    return;
  case CK_NullToPointer:
    getOriginsList(*CE);
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
        Dest->getOuterOriginID(), Src->getOuterOriginID(), /*Kill=*/true));
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

void FactsGenerator::VisitReturnStmt(const ReturnStmt *RS) {
  if (const Expr *RetExpr = RS->getRetValue()) {
    if (OriginList *List = getOriginsList(*RetExpr))
      for (OriginList *L = List; L != nullptr; L = L->peelOuterOrigin())
        EscapesInCurrentBlock.push_back(FactMgr.createFact<ReturnEscapeFact>(
            L->getOuterOriginID(), RetExpr));
  }
}

void FactsGenerator::handleAssignment(const Expr *TargetExpr,
                                      const Expr *LHSExpr,
                                      const Expr *RHSExpr) {
  LHSExpr = LHSExpr->IgnoreParenImpCasts();
  OriginList *LHSList = nullptr;

  if (const auto *DRE_LHS = dyn_cast<DeclRefExpr>(LHSExpr)) {
    LHSList = getOriginsList(*DRE_LHS);
    assert(LHSList && "LHS is a DRE and should have an origin list");
  }
  // Handle assignment to member fields (e.g., `this->view = s` or `view = s`).
  // This enables detection of dangling fields when local values escape to
  // fields.
  if (const auto *ME_LHS = dyn_cast<MemberExpr>(LHSExpr)) {
    LHSList = getOriginsList(*ME_LHS);
    assert(LHSList && "LHS is a MemberExpr and should have an origin list");
  }
  // Assignment to an array element (`arr[i] = &x`). All elements share the
  // array's single element-origin, so we cannot tell which element is
  // overwritten: merge the new loans in rather than killing the old ones (the
  // origin conservatively holds the loans of every element ever stored).
  bool MergeIntoSharedElement = false;
  if (const auto *ASE_LHS = dyn_cast<ArraySubscriptExpr>(LHSExpr);
      ASE_LHS &&
      ASE_LHS->getBase()->IgnoreParenImpCasts()->getType()->isArrayType()) {
    LHSList = getOriginsList(*ASE_LHS);
    MergeIntoSharedElement = LHSList != nullptr;
  }
  if (!LHSList)
    return;
  OriginList *RHSList = getOriginsList(*RHSExpr);
  // For operator= with reference parameters (e.g.,
  // `View& operator=(const View&)`), the RHS argument stays an lvalue,
  // unlike built-in assignment where LValueToRValue cast strips the outer
  // lvalue origin. Strip it manually to get the actual value origins being
  // assigned.
  RHSList = getRValueOrigins(RHSExpr, RHSList);

  if (const auto *DRE_LHS = dyn_cast<DeclRefExpr>(LHSExpr)) {
    QualType QT = DRE_LHS->getDecl()->getType();
    if (QT->isReferenceType()) {
      if (hasOrigins(QT->getPointeeType())) {
        // Writing through a reference uses the binding but overwrites the
        // pointee. Model this as a Read of the outer origin (keeping the
        // binding live) and a Write of the inner origins (killing the pointee's
        // liveness).
        if (UseFact *UF = UseFacts.lookup(DRE_LHS)) {
          const OriginList *FullList = UF->getUsedOrigins();
          assert(FullList);
          UF->setUsedOrigins(FactMgr.getOriginMgr().createSingleOriginList(
              FullList->getOuterOriginID()));
          if (const OriginList *InnerList = FullList->peelOuterOrigin()) {
            UseFact *WriteUF = FactMgr.createFact<UseFact>(DRE_LHS, InnerList);
            WriteUF->markAsWritten();
            CurrentBlockFacts.push_back(WriteUF);
          }
        }
      }
    } else
      markUseAsWrite(DRE_LHS);
  }
  if (!RHSList) {
    // RHS has no tracked origins (e.g., assigning a callable without origins
    // to std::function). Clear loans of the destination.
    for (OriginList *LHSInner = LHSList->peelOuterOrigin(); LHSInner;
         LHSInner = LHSInner->peelOuterOrigin())
      CurrentBlockFacts.push_back(
          FactMgr.createFact<KillOriginFact>(LHSInner->getOuterOriginID()));
    return;
  }
  // Kill the old loans of the destination origin and flow the new loans
  // from the source origin. For a shared array element-origin we merge instead
  // of killing (see above).
  flow(LHSList->peelOuterOrigin(), RHSList, /*Kill=*/!MergeIntoSharedElement);
  killAndFlowOrigin(*TargetExpr, *LHSExpr);
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
  // For list initialization with a single element, like `View{...}`, the
  // origin of the list itself is the origin of its single element.
  if (ILE->getNumInits() == 1)
    killAndFlowOrigin(*ILE, *ILE->getInit(0));
}

void FactsGenerator::VisitCXXBindTemporaryExpr(
    const CXXBindTemporaryExpr *BTE) {
  killAndFlowOrigin(*BTE, *BTE->getSubExpr());
}

void FactsGenerator::VisitMaterializeTemporaryExpr(
    const MaterializeTemporaryExpr *MTE) {
  assert(MTE->isGLValue());
  OriginList *MTEList = getOriginsList(*MTE);
  if (!MTEList)
    return;
  OriginList *SubExprList = getOriginsList(*MTE->getSubExpr());
  assert((!SubExprList ||
          MTEList->getLength() == (SubExprList->getLength() + 1)) &&
         "MTE top level origin should contain a loan to the MTE itself");

  OriginList *RValMTEList = getRValueOrigins(MTE, MTEList);
  flow(RValMTEList, SubExprList, /*Kill=*/true);
  OriginID OuterMTEID = MTEList->getOuterOriginID();
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
  OriginList *LambdaList = getOriginsList(*LE);
  if (!LambdaList)
    return;
  bool Kill = true;
  for (const Expr *Init : LE->capture_inits()) {
    if (!Init)
      continue;
    OriginList *InitList = getOriginsList(*Init);
    if (!InitList)
      continue;
    // FIXME: Consider flowing all origin levels once lambdas support more than
    // one origin. Currently only the outermost origin is flowed, so by-ref
    // captures like `[&p]` (where p is string_view) miss inner-level
    // invalidation.
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        LambdaList->getOuterOriginID(), InitList->getOuterOriginID(), Kill));
    Kill = false;
  }
}

void FactsGenerator::VisitArraySubscriptExpr(const ArraySubscriptExpr *ASE) {
  assert(ASE->isGLValue() && "Array subscript should be a GL value");
  OriginList *Dst = getOriginsList(*ASE);
  assert(Dst && "Array subscript should have origins as it is a GL value");
  OriginList *Src = getOriginsList(*ASE->getBase());
  assert(Src && "Base of array subscript should have origins");
  CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
      Dst->getOuterOriginID(), Src->getOuterOriginID(), /*Kill=*/true));
}

void FactsGenerator::handlePlacementNew(const CXXNewExpr *NE,
                                        OriginList *NewList) {
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
  OriginList *PlacementList = getOriginsList(*PlacementArg);
  // FIXME: General placement arguments need separate handling to overwrite
  // the right origins.

  // The pointer returned by placement new comes from the placement
  // argument.
  if (PlacementList)
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        NewList->getOuterOriginID(), PlacementList->getOuterOriginID(), true));
}

void FactsGenerator::VisitCXXNewExpr(const CXXNewExpr *NE) {
  OriginList *NewList = getOriginsList(*NE);
  const Expr *Init = NE->getInitializer();

  if (NE->getNumPlacementArgs() == 1) {
    handlePlacementNew(NE, NewList);
  } else {
    const Loan *L = createLoan(FactMgr, NE);
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), NewList->getOuterOriginID()));
  }

  NewList = NewList->peelOuterOrigin();

  if (!NewList || !Init)
    return;

  // FIXME: OriginList is null for `new[]` initializers. Remove this `Init`
  // check once array origins are supported.
  if (OriginList *InitList = getOriginsList(*Init); InitList)
    flow(NewList, InitList, true);
}

void FactsGenerator::VisitCXXDeleteExpr(const CXXDeleteExpr *DE) {
  OriginList *List = getOriginsList(*DE->getArgument());
  CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
      List->getOuterOriginID(), DE, /*Assumed=*/false, /*Deallocation=*/true));
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
  if (OriginList *List = getOriginsList(*LifetimeEndsVD)) {
    OriginID OID = List->getOuterOriginID();
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
  if (CCE->getNumArgs() != 1)
    return;

  const Expr *Arg = CCE->getArg(0);
  if (isGslPointerType(Arg->getType())) {
    OriginList *ArgList = getOriginsList(*Arg);
    assert(ArgList && "GSL pointer argument should have an origin list");
    // GSL pointer is constructed from another gsl pointer.
    // Example:
    //  View(View v);
    //  View(const View &v);
    ArgList = getRValueOrigins(Arg, ArgList);
    flow(getOriginsList(*CCE), ArgList, /*Kill=*/true);
  } else if (Arg->getType()->isPointerType()) {
    // GSL pointer is constructed from a raw pointer. Flow only the outermost
    // raw pointer. Example:
    //  View(const char*);
    //  Span<int*>(const in**);
    OriginList *ArgList = getOriginsList(*Arg);
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        getOriginsList(*CCE)->getOuterOriginID(), ArgList->getOuterOriginID(),
        /*Kill=*/true));
  } else {
    // This could be a new borrow.
    // TODO: Add code example here.
    handleFunctionCall(CCE, CCE->getConstructor(),
                       {CCE->getArgs(), CCE->getNumArgs()},
                       /*IsGslConstruction=*/true);
  }
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
      OriginList *MovedOrigins = getOriginsList(*UniquePtrExpr);
      if (MovedOrigins)
        CurrentBlockFacts.push_back(FactMgr.createFact<MovedOriginFact>(
            UniquePtrExpr, MovedOrigins->getOuterOriginID()));
    }
  }

  // Skip 'this' arg as it cannot be moved.
  for (unsigned I = IsInstance;
       I < Args.size() && I < FD->getNumParams() + IsInstance; ++I) {
    const ParmVarDecl *PVD = FD->getParamDecl(I - IsInstance);
    if (!PVD->getType()->isRValueReferenceType())
      continue;
    const Expr *Arg = Args[I];
    OriginList *MovedOrigins = getOriginsList(*Arg);
    assert(MovedOrigins->getLength() >= 1 &&
           "unexpected length for r-value reference param");
    // Arg is being moved to this parameter. Mark the origin as moved.
    CurrentBlockFacts.push_back(FactMgr.createFact<MovedOriginFact>(
        Arg, MovedOrigins->getOuterOriginID()));
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

  // Heuristics to turn-down false positives. Skip member field expressions for
  // now. This is not a perfect filter and will still surface some false
  // positives (e.g. `auto& r = s.v`).
  if (!isa<DeclRefExpr>(Args[0]->IgnoreImpCasts()))
    return;

  OriginList *ThisList = getOriginsList(*Args[0]);
  if (ThisList)
    CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
        ThisList->getOuterOriginID(), Call));
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

  // (1) Non-const member call on an owner receiver. Known container mutators
  // are handled precisely elsewhere; borrow-returning accessors (recognized GSL
  // accessors or methods that are lifetimebound on 'this') do not invalidate.
  if (IsInstance && !Method->isConst() && !isInvalidationMethod(*Method) &&
      !Args.empty() && isGslOwnerType(Args[0]->getType()) &&
      !shouldTrackImplicitObjectArg(*Args[0], Method,
                                    /*RunningUnderLifetimeSafety=*/true) &&
      !implicitObjectParamIsLifetimeBound(Method)) {
    if (OriginList *L = getOriginsList(*Args[0]))
      CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
          L->getOuterOriginID(), Call, /*Assumed=*/true));
  }

  // (2) Passing an owner to a non-const pointer/reference parameter. The
  // implicit object argument (I == 0 for instance methods) is intentionally
  // skipped here -- it is handled by case (1) above.
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
    QualType PT = PVD->getType();
    // The parameter is assumed to mutate the owner the argument refers to if it
    // is a non-const pointer/reference to an owner, or a gsl::Pointer (e.g.
    // std::span) that exposes mutable access to a non-const owner pointee.
    bool MutatesOwner = false;
    if (PT->isPointerType()) {
      QualType Pointee = PT->getPointeeType();
      MutatesOwner = !Pointee.isConstQualified() && isGslOwnerType(Pointee);
    } else if (isGslPointerType(PT.getNonReferenceType())) {
      MutatesOwner = pointsToMutableOwner(PT.getNonReferenceType());
    } else if (PT->isReferenceType()) {
      QualType Pointee = PT->getPointeeType();
      MutatesOwner = !Pointee.isConstQualified() && isGslOwnerType(Pointee);
    }
    if (!MutatesOwner)
      continue;
    if (OriginList *L = getOriginsList(*Args[I]))
      CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
          L->getOuterOriginID(), Call, /*Assumed=*/true));
  }
}

void FactsGenerator::handleDestructiveCall(const Expr *Call,
                                           const FunctionDecl *FD,
                                           ArrayRef<const Expr *> Args) {
  if (!destructsFirstArg(*FD))
    return;
  OriginList *ArgList = getOriginsList(*Args[0]);
  if (ArgList)
    CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
        ArgList->getOuterOriginID(), Call, /*Assumed=*/false,
        /*Deallocation=*/true));
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
      if (auto *FieldList = getOriginsList(*Field))
        CurrentBlockFacts.push_back(
            FactMgr.createFact<UseFact>(Call, FieldList));
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
    OriginList *CapturedOriginList = getOriginsList(*Args[I]);
    if (!CapturedOriginList)
      continue;
    if (!CapturedOriginList)
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

      OriginList *CapturingOriginList = getOriginsList(*CapturedByArg);
      OriginList *Dest = getRValueOrigins(CapturedByArg, CapturingOriginList);
      if (!Dest)
        continue;
      // KillDest=false because we cannot know if previous captures are being
      // replaced or accumulated. Multiple successive captures into the same
      // destination must all be tracked, so captured lifetimes are always
      // merged.
      CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
          Dest->getOuterOriginID(), CapturedOriginList->getOuterOriginID(),
          /*KillDest=*/false));
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
    //
    // It never applies to arbitrary user constructors, which may capture.
    if (Method) {
      QualType ParamTy = PVD->getType();
      bool IsContainerCtor = isa<CXXConstructorDecl>(Method) &&
                             isStlContainerType(Method->getParent());
      if (ParamTy->isReferenceType() &&
          !hasOrigins(ParamTy.getNonReferenceType()) &&
          (isStlContainerInsertionMethod(*Method) || IsContainerCtor))
        continue;
      if (IsContainerCtor && ParamTy->isPointerType() &&
          !hasOrigins(ParamTy->getPointeeType()))
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
  OriginList *CallList = getOriginsList(*Call);
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
  handleMovedArgsInCall(FD, Args);
  handleImplicitObjectFieldUses(Call, FD);
  handleLifetimeCaptureBy(FD, Args);
  handleUnannotatedIndirectionArgs(FD, Args);
  handleMoveSilencing(Call, FD, Args);
  if (!CallList)
    return;
  // A [[clang::lifetime_immortal]] function returns storage that lives for the
  // whole program. Model it as a borrow of immortal storage that never expires,
  // independent of the arguments.
  if (FD->hasAttr<clang::LifetimeImmortalAttr>()) {
    const Loan *L =
        FactMgr.getLoanMgr().createLoan(AccessPath::Immortal(FD), Call);
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), CallList->getOuterOriginID()));
    return;
  }
  // A function with __attribute__((malloc)) returns a fresh heap allocation.
  // Model it like `new` so the result is tracked and a later free/delete of it
  // is not reported as a naked deallocation.
  if (FD->hasAttr<clang::RestrictAttr>()) {
    const Loan *L = FactMgr.getLoanMgr().createLoan(
        AccessPath::HeapAllocation(Call), Call);
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), CallList->getOuterOriginID()));
    return;
  }
  if (isStdReferenceCast(FD)) {
    assert(Args.size() == 1 &&
           "std reference cast builtins take exactly one argument");
    // std reference-cast functions like std::move return a result that refers
    // to the same object as the argument, so propagate the full origins.
    flow(CallList, getOriginsList(*Args[0]), /*Kill=*/true);
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
    OriginList *ArgList = getOriginsList(*Args[I]);
    if (!ArgList)
      continue;
    if (IsGslConstruction) {
      // TODO: document with code example.
      // std::string_view(const std::string_view& from)
      if (isGslPointerType(Args[I]->getType())) {
        assert(!Args[I]->isGLValue() || ArgList->getLength() >= 2);
        ArgList = getRValueOrigins(Args[I], ArgList);
      }
      if (isGslOwnerType(Args[I]->getType())) {
        // The constructed gsl::Pointer borrows from the Owner's storage, not
        // from what the Owner itself borrows, so only the outermost origin is
        // needed.
        CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
            CallList->getOuterOriginID(), ArgList->getOuterOriginID(),
            KillSrc));
        KillSrc = false;
      } else if (IsArgLifetimeBound(I)) {
        // Only flow the outer origin here. For lifetimebound args in
        // gsl::Pointer construction, we do not have enough information to
        // safely match inner origins, so the source and
        // destination origin lists may have different lengths.
        // FIXME: Handle origin-shape mismatches gracefully so we can also flow
        // inner origins.
        CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
            CallList->getOuterOriginID(), ArgList->getOuterOriginID(),
            KillSrc));
        KillSrc = false;
      }
    } else if (shouldTrackPointerImplicitObjectArg(I)) {
      assert(ArgList->getLength() >= 2 &&
             "Object arg of pointer type should have at least two origins");
      // See through the GSLPointer reference to see the pointer's value.
      CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
          CallList->getOuterOriginID(),
          ArgList->peelOuterOrigin()->getOuterOriginID(), KillSrc));
      KillSrc = false;
    } else if (IsArgLifetimeBound(I)) {
      // Lifetimebound on a non-GSL-ctor function means the returned
      // pointer/reference itself must not outlive the arguments. This
      // only constrains the top-level origin.
      CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
          CallList->getOuterOriginID(), ArgList->getOuterOriginID(), KillSrc));
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
  OriginList *List = getOriginsList(*E);
  if (!List)
    return;
  // For DeclRefExpr: Remove the outer layer of origin which borrows from the
  // decl directly (e.g., when this is not a reference). This is a use of the
  // underlying decl.
  if (auto *DRE = dyn_cast<DeclRefExpr>(E);
      DRE && !DRE->getDecl()->getType()->isReferenceType())
    List = getRValueOrigins(DRE, List);
  // Skip if there is no inner origin (e.g., when it is not a pointer type).
  if (!List)
    return;
  if (!UseFacts.contains(E)) {
    UseFact *UF = FactMgr.createFact<UseFact>(E, List);
    CurrentBlockFacts.push_back(UF);
    UseFacts[E] = UF;
  }
}

void FactsGenerator::markUseAsWrite(const DeclRefExpr *DRE) {
  if (UseFacts.contains(DRE))
    UseFacts[DRE]->markAsWritten();
}

// Creates an IssueFact for a new placeholder loan for each pointer or reference
// parameter at the function's entry.
llvm::SmallVector<Fact *> FactsGenerator::issuePlaceholderLoans() {
  const auto *FD = dyn_cast<FunctionDecl>(AC.getDecl());
  if (!FD)
    return {};

  llvm::SmallVector<Fact *> PlaceholderLoanFacts;
  if (auto ThisOrigins = FactMgr.getOriginMgr().getThisOrigins()) {
    OriginList *List = *ThisOrigins;
    const Loan *L = FactMgr.getLoanMgr().createLoan(
        AccessPath::Placeholder(cast<CXXMethodDecl>(FD)),
        /*IssuingExpr=*/nullptr);
    PlaceholderLoanFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), List->getOuterOriginID()));
  }
  for (const ParmVarDecl *PVD : FD->parameters()) {
    OriginList *List = getOriginsList(*PVD);
    if (!List)
      continue;
    const Loan *L = FactMgr.getLoanMgr().createLoan(
        AccessPath::Placeholder(PVD), /*IssuingExpr=*/nullptr);
    PlaceholderLoanFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), List->getOuterOriginID()));
  }
  return PlaceholderLoanFacts;
}

} // namespace clang::lifetimes::internal
