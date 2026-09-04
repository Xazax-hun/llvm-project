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
#include "llvm/ADT/ScopeExit.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/StmtObjC.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/Analysis/Analyses/LifetimeSafety/FactsGenerator.h"
#include "clang/Basic/SourceManager.h"
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
///
/// \param Block Optional. If provided, the generated flow facts are appended to
///              this specific CFG block instead of the block being visited. Used
///              to path-isolate a conditional operator's arms (see
///              VisitConditionalOperator).
void FactsGenerator::flow(OriginNode *Dst, OriginNode *Src, bool Kill,
                          const CFGBlock *Block) {
  if (!Dst)
    return;
  assert(Src &&
         "Dst is non-null but Src is null. List must have the same length");
  assert(Dst->getLength() == Src->getLength() &&
         "Pointee chains must have the same length");

  while (Dst && Src) {
    Fact *F = FactMgr.createFact<OriginFlowFact>(Dst->getOriginID(),
                                                 Src->getOriginID(), Kill);
    if (Block)
      FactMgr.appendBlockFact(Block, F);
    else
      CurrentBlockFacts.push_back(F);
    for (const OriginNode::Edge &E : Dst->children())
      if (E.FD)
        if (OriginNode *SrcF = Src->getFieldChild(E.FD))
          flow(E.Child, SrcF, Kill, Block);
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

/// True if the callee can mutate anything through a parameter of type `PT` at all:
/// it must be a non-const pointer or reference. This is the only thing the
/// *parameter* decides for the assumed-invalidation gate; whether an owner is
/// actually reachable is confirmed from the loans the argument carries
/// (OwnerLoanGate::DenotedOwner), not from any static type. A static type cannot
/// answer it: the parameter's may be a base that hides the owner, and so may the
/// argument's if the upcast happened earlier (`Base &b = derived; f(b);`).
static bool paramCanMutateThrough(QualType PT) {
  if (!PT->isPointerType() && !PT->isReferenceType())
    return false;
  return !PT->getPointeeType().isConstQualified();
}

/// Returns true if a parameter of type `PT` lets the call mutate the owner the
/// argument refers to: a non-const pointer/reference to an owner (or to a record
/// that transitively contains a mutable owner field), or a gsl::Pointer that
/// exposes mutable access to a non-const owner pointee. Shared by the assumed-
/// invalidation and argument-overlap checks.
static bool paramMayMutateOwner(QualType PT) {
  if (PT->isPointerType() || PT->isReferenceType()) {
    // A pointer or reference to an ARRAY reaches the elements, so the element
    // type decides the hazard: `std::string (&)[2]` lets the callee reallocate
    // `arr[0]` exactly as `std::string &` does. Peel before every test below,
    // including the const one -- an array of const elements carries the
    // qualifier on the element type, not on the array.
    QualType Pointee = arrayElementType(PT->getPointeeType());
    if (Pointee.isConstQualified())
      return false;
    if (isGslOwnerType(Pointee))
      return true;
    // An OPAQUE pointee cannot be shown to be owner-free, so it must not be
    // assumed to be. `void *` is the C-interop "userdata" idiom: the callee casts
    // it back to the real type and can mutate an owner through it, while the
    // signature reveals nothing. An INCOMPLETE record is the same situation --
    // the forward-declared opaque-handle idiom -- and it is worse for being
    // order-dependent: the type may be completed later in the TU, so whether the
    // hazard is visible would otherwise depend on where the analysis runs.
    if (Pointee->isVoidType())
      return true;
    if (const CXXRecordDecl *RD = Pointee->getAsCXXRecordDecl();
        RD && !RD->hasDefinition())
      return true;
    return recordContainsMutableOwner(Pointee->getAsCXXRecordDecl());
  }
  if (isGslPointerType(PT.getNonReferenceType())) {
    if (pointsToMutableOwner(PT.getNonReferenceType()))
      return true;
    // A gsl::Pointer wrapper that reaches a mutable owner through a member it
    // aliases (e.g. a `std::vector<int>* v` member). A by-value copy still
    // aliases the same owner, so the callee can reallocate it through the copy,
    // invalidating borrows into it. (A by-value record that OWNS its owner is a
    // copy and must not be treated this way -- hence gated on gsl::Pointer.)
    return recordContainsMutableOwner(
        PT.getNonReferenceType()->getAsCXXRecordDecl());
  }
  // A BY-VALUE record that ALIASES a mutable owner: a lambda capturing a
  // container by reference, a std::function wrapping one, a struct holding a
  // `vector<T>&`. Copying it copies the alias, so calling it can still
  // reallocate the CALLER's owner -- the same reasoning as the gsl::Pointer
  // case above, and the reason a callback parameter is a mutation the signature
  // does not otherwise show. A by-value record that OWNS its owner is excluded,
  // since the copy owns its own.
  return recordAliasesMutableOwner(PT->getAsCXXRecordDecl());
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

/// Returns true if `MD` is known not to invalidate borrows into the object it is
/// called on: either it carries `[[clang::lifetime_non_invalidating]]`, or it is
/// a recognized non-invalidating accessor of a standard library type -- one that
/// returns a borrow into the container (or smart pointer pointee) without
/// reallocating it. Non-const member calls on an owner are otherwise
/// conservatively assumed to invalidate; this keeps the common read accessors
/// (`v[i]`, `v.at(i)`, `v.data()`, `m.find(k)`, `*p`, ...) from being treated as
/// mutating. The std allow-list is by name and so cannot cover user types; those
/// use the attribute.
static bool isNonInvalidatingMethod(const CXXMethodDecl &MD) {
  // An explicit promise from the author covers user-defined owners, whose
  // accessors the name-based allow-list below cannot recognize.
  if (MD.hasAttr<LifetimeNonInvalidatingAttr>())
    return true;
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
  // ...and the code must be the library's OWN. A user specialization of a standard
  // template -- `template <> struct std::hash<Tag>` -- is legal, conforming C++ and
  // sits in the real `std` namespace, so the namespace test alone handed this
  // allow-list to user-written methods: one merely NAMED `data` was exempted from
  // assumed invalidation even while it reallocated.
  if (!MD.getASTContext().getSourceManager().isInSystemHeader(MD.getLocation()))
    return false;
  switch (MD.getOverloadedOperator()) {
  case OO_Subscript: // operator[]
  case OO_Star:      // operator* (smart pointers, iterators)
  case OO_Arrow:     // operator->
    return true;
  // Iterator traversal mutates the ITERATOR, never the container it points
  // into, so it cannot invalidate a borrow of that container. These are
  // non-const by necessity (they update the iterator's own position), so
  // without this a plain read-only loop over a member container looks like a
  // mutation of the enclosing object: the iterator carries the container's
  // loan, and `++it` is a non-const call on something holding it.
  case OO_PlusPlus:   // ++it / it++
  case OO_MinusMinus: // --it / it--
  case OO_PlusEqual:  // it += n
  case OO_MinusEqual: // it -= n
    return isGslPointerType(
        MD.getASTContext().getCanonicalTagType(MD.getParent()));
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
      else if (std::optional<CFGMemberDtor> MemberDtor =
                   Element.getAs<CFGMemberDtor>())
        handleMemberDtor(*MemberDtor);
      else if (std::optional<CFGBaseDtor> BaseDtor =
                   Element.getAs<CFGBaseDtor>())
        handleBaseDtor(*BaseDtor);
      else if (std::optional<CFGFullExprCleanup> FullExprCleanup =
                   Element.getAs<CFGFullExprCleanup>()) {
        handleFullExprCleanup(*FullExprCleanup);
      } else if (std::optional<CFGTemporaryDtor> TemporaryDtor =
                     Element.getAs<CFGTemporaryDtor>())
        handleTemporaryDtor(*TemporaryDtor);
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

void FactsGenerator::handleGlobalContainerOfIndirectionUse(const Expr *UseExpr,
                                                           const VarDecl *VD) {
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
  CurrentBlockFacts.push_back(
      FactMgr.createFact<UntrackedConstructFact>(Reason, UseExpr, ReportType));
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
  if (const Expr *Init = DIE->getExpr()) {
    // The subexpression is in the CFG -- AddCXXDefaultInitExprInAggregates asks for it
    // -- so everything inside is visited normally, through a constructor or through
    // aggregate initialization alike. Nothing has to be replayed here.
    killAndFlowOrigin(*DIE, *Init);
  }
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
  // A STATIC data member accessed through an object (`r.slot`) is a use of the
  // variable itself -- the base is evaluated and discarded. getOrCreateNode
  // shares the variable's origins for this spelling, so all that is left is to
  // do what VisitDeclRefExpr does for the qualified spelling: issue the loan on
  // the variable's storage, and register the use. Without the loan a store of a
  // local's address into the member escapes unreported, because the
  // global-escape fact at exit needs a loan to carry.
  if (auto *SVD = dyn_cast<VarDecl>(ME->getMemberDecl());
      SVD && SVD->hasGlobalStorage()) {
    handleUse(ME);
    handleGlobalContainerOfIndirectionUse(ME, SVD);
    if (doesDeclHaveStorage(SVD)) {
      AccessPath Path(SVD);
      const Loan *L = FactMgr.getLoanMgr().createLoan(Path, ME);
      OriginNode *Node = getOriginNode(*ME);
      assert(Node && "gl-value member of non-pointer type should have origins");
      CurrentBlockFacts.push_back(
          FactMgr.createFact<IssueFact>(L->getID(), Node->getOriginID()));
    }
    return;
  }

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

    // The field's glvalue (outermost origin) holds the same loans as the base
    // expression...
    CurrentBlockFacts.push_back(FactMgr.createFact<OriginFlowFact>(
        Dst->getOriginID(), Src->getOriginID(),
        /*Kill=*/true));
    // ...extended by this field, so `w.c` denotes `w.c` rather than merely `w`.
    // Two accesses of different fields of one object then hold distinct loans,
    // which is what lets a mutation of one be told apart from a borrow of the
    // other.
    //
    // This is also what makes a borrow of an OWNER field (e.g. `this->buf` where
    // `buf` is a std::string/std::vector) denote the field's heap buffer, so a
    // later mutation of the field -- directly (`buf.append(...)`) or via a
    // non-const method on the containing object -- can invalidate views into it.
    // Projecting the base rather than rooting a fresh loan at the FieldDecl
    // keeps the whole prefix (`w.c.buf`, not `buf`), which is what lets the
    // enclosing object be recovered and disjoint siblings be told apart.
    CurrentBlockFacts.push_back(FactMgr.createFact<ProjectionFact>(
        Dst->getOriginID(), PathElement::getField(*FD), ME));
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

/// True if \p CE recovers a typed pointer out of a `void *`.
///
/// Like a `reinterpret_cast` this hides where the pointer came from: `void *` is
/// opaque, so the conversion can name any type, and splitting a conversion in two
/// through one launders it past any check that compares the source and target types
/// -- `Base * -> void * -> Derived *` has a record on only one side of each half.
///
/// Casting *to* `void *` is not reported: that is the opaque-userdata idiom, and
/// what a callee may do with such a parameter is already handled conservatively
/// (paramMayMutateOwner treats a `void` pointee as possibly reaching an owner).
static bool isCastFromVoidPointer(const CastExpr *CE) {
  QualType From = CE->getSubExpr()->getType();
  if (!From->isPointerType() || !From->getPointeeType()->isVoidType())
    return false;
  QualType To = CE->getType();
  // Only recovering a TYPED pointer loses something; `void *` to `void *` (adding
  // cv-qualification, say) does not.
  return (To->isPointerType() || To->isReferenceType()) &&
         !To->getPointeeType()->isVoidType();
}

/// True if \p CE converts a base class pointer/reference to a derived one.
///
/// Keyed on the TYPES, not on the cast kind: a `static_cast`, a C-style cast and a
/// `reinterpret_cast` all reach the same subobject, and the operand may be `this`,
/// a parameter, or a pointer loaded from the heap. (A `reinterpret_cast` is refused
/// on its own account anyway.)
///
/// `dynamic_cast` is excluded. It is checked at run time, and inside a constructor
/// or destructor the object is treated as being of that constructor's or
/// destructor's own class, so a conversion to a derived type is well defined and
/// simply fails -- which makes it the way to write a checked downcast.
static bool isDowncast(const CastExpr *CE) {
  if (CE->getCastKind() == CK_Dynamic)
    return false;
  auto Pointee = [](QualType T) {
    T = T.getNonReferenceType();
    if (T->isPointerType())
      T = T->getPointeeType();
    return T;
  };
  // A POINTER TO MEMBER runs the other way round. Converting `Derived::*` to
  // `Base::*` is what lets a Base object reach a member that only exists on
  // Derived (`b->*p`), so that is the hazardous direction -- the mirror of the
  // object-pointer case. `Base::* -> Derived::*` is the implicit, safe widening: a
  // member of Base is a member of every Derived.
  //
  // This needs its own test because a member-pointer type has no pointee class to
  // peel: the peeling below yields the POINTED-TO type (`std::string`), not the
  // class, so both sides came back null and the conversion was never examined.
  if (const auto *ToMP = CE->getType()->getAs<MemberPointerType>())
    if (const auto *FromMP =
            CE->getSubExpr()->getType()->getAs<MemberPointerType>()) {
      const CXXRecordDecl *ToCls = ToMP->getMostRecentCXXRecordDecl();
      const CXXRecordDecl *FromCls = FromMP->getMostRecentCXXRecordDecl();
      return ToCls && FromCls && ToCls->hasDefinition() &&
             FromCls->hasDefinition() &&
             ToCls->getCanonicalDecl() != FromCls->getCanonicalDecl() &&
             FromCls->isDerivedFrom(ToCls);
    }

  const CXXRecordDecl *To = Pointee(CE->getType())->getAsCXXRecordDecl();
  const CXXRecordDecl *From =
      Pointee(CE->getSubExpr()->getType())->getAsCXXRecordDecl();
  if (!To || !From || !To->hasDefinition() || !From->hasDefinition())
    return false;
  if (To->getCanonicalDecl() == From->getCanonicalDecl())
    return false;
  return To->isDerivedFrom(From);
}

/// True if \p CE reinterprets the bytes of one type as another -- what a
/// `reinterpret_cast` does, however it happens to be spelled.
///
/// Keyed on the cast KIND rather than the spelling. `(T *)p` and
/// `reinterpret_cast<T *>(p)` produce the identical `BitCast`, so testing for
/// CXXReinterpretCastExpr caught only the second: a C-style cast performing the
/// byte-identical reinterpretation walked straight through, on a global array or a
/// local buffer alike.
///
/// Only WRITTEN casts count. An implicit BitCast is the front end's own bookkeeping --
/// converting to `void *` nests one inside the explicit NoOp cast -- and flagging those
/// would report every such conversion.
///
/// A conversion involving `void *` is left to isCastFromVoidPointer: casting TO one is
/// the opaque-userdata idiom and is allowed, and recovering FROM one has its own report.
static bool reinterpretsBytes(const CastExpr *CE) {
  if (!isa<ExplicitCastExpr>(CE))
    return false;
  switch (CE->getCastKind()) {
  case CK_BitCast:
  case CK_LValueBitCast:
  case CK_ReinterpretMemberPointer:
    break;
  default:
    return false;
  }
  auto IsVoidPointer = [](QualType T) {
    T = T.getNonReferenceType();
    return T->isPointerType() && T->getPointeeType()->isVoidType();
  };
  return !IsVoidPointer(CE->getType()) &&
         !IsVoidPointer(CE->getSubExpr()->getType());
}

void FactsGenerator::VisitCastExpr(const CastExpr *CE) {
  // Safe-model soundness: reinterpreting one type's bytes as another can launder a
  // borrow through an unrelated type, hiding its provenance, so a borrow recovered
  // that way is not tracked. Surface it as unsupported however it is spelled -- the
  // `reinterpret_cast` keyword, or a C-style or functional cast that does the same
  // thing. (The keyword is also tested directly, since a reinterpret_cast between a
  // pointer and an integer uses a different cast kind.)
  if (isa<CXXReinterpretCastExpr>(CE) || reinterpretsBytes(CE))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::ReinterpretCast, cast<Expr>(CE)));

  // Likewise a base-to-derived conversion. Whether the derived subobject is alive
  // depends on whether the complete object's construction has finished and its
  // destruction has not begun, and nothing at the conversion reveals that: inside
  // a constructor or destructor of a base it is undefined ([class.cdtor]), and a
  // base destructor reaching derived state is a use-after-free, since bases are
  // destroyed after the derived part. The analysis models `this` as a live
  // complete object, so every step -- the conversion, the member access, the read
  // -- looks perfectly modelable and no loan ever expires. Refuse the construct
  // instead, the way inline asm, setjmp and reinterpret_cast are refused.
  if (isDowncast(CE))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::Downcast, cast<Expr>(CE)));

  // And recovering a typed pointer out of a `void *`, which launders provenance
  // the same way -- and is how a base-to-derived conversion evades the test above,
  // by going through `void *` so neither half has a record on both sides.
  if (isCastFromVoidPointer(CE))
    CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
        UntrackedConstructReason::VoidPointerCast, cast<Expr>(CE)));

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
    // Dereferencing reads the pointer's VALUE in order to follow it, so this is
    // a use of the borrow the operand holds -- including for a write through the
    // deref (`*p = x`), where the pointee is overwritten but `p` itself is still
    // read. A read of a pointer *member* registers no use of its own
    // (VisitMemberExpr does not call handleUse, and `this` supplies no UseFact to
    // narrow), so without this a borrow held in a member and then dereferenced is
    // not live where the loan sits, and an invalidation before it is missed.
    handleUse(SubExpr);
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
    // The destination is not a single statically-known origin. That happens for
    // an lvalue that selects or forwards among several objects -- a conditional
    // `(c ? p : q) = ...`, a comma `(f(), p) = ...`, a derived-to-base
    // conversion of a receiver, or those wrapped in `*&(...)`/casts.
    //
    // Which storage such an lvalue designates is exactly what its OWN loans
    // say, and those are only known after loan propagation -- so the routing is
    // deferred to a DynamicStoreFact rather than decided here. The checker
    // refuses the store if those loans turn out not to name resolvable storage,
    // which is what the blanket refusal used to do for every spelling.
    //
    // Gated on the destination TYPE holding a borrow (a pointer/view), not on
    // hasOrigins(Expr): the latter is true of every glvalue, so it would also
    // cover a plain `cells[i] = ' '` char store through operator[].
    if (hasOrigins(LHSExpr->getType()))
      if (OriginNode *DestLV = getOriginNode(*LHSExpr))
        // Same r-value peel the routed path below applies to the stored value.
        if (OriginNode *Src =
                getRValueOrigins(RHSExpr, getOriginNode(*RHSExpr)))
          CurrentBlockFacts.push_back(FactMgr.createFact<DynamicStoreFact>(
              DestLV->getOriginID(), Src->getOriginID(), cast<Expr>(LHSExpr)));
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
        // For an ARROW access the container is what the base points AT: `n->d`
        // stores into `*n`, not into the pointer variable `n`. A pointer
        // variable has storage of its own, so its lvalue origin carries a loan
        // naming that variable while the caller's object -- the parameter
        // placeholder -- lives on the POINTEE origin. Using the pointer's own
        // origin made the store look like it targeted the local pointer, so a
        // noescape borrow stored through `n->d` escaped unreported while the
        // same store through a reference (`n.d`, where a reference has no
        // storage and its lvalue origin IS the object's) was caught.
        //
        // `this` is excluded: the model already gives it the OBJECT's origin,
        // not a pointer's, so descending there would go a level too deep.
        if (ME_LHS->isArrow() && !isa<CXXThisExpr>(Base->IgnoreParenImpCasts()))
          if (OriginNode *Pointee = Container->getPointeeChild())
            Container = Pointee;
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
    // A compound assignment reads AND writes its left operand, and both happen
    // AFTER the right operand is evaluated -- so if the RHS invalidates what
    // the LHS borrows, the write goes through a dangling borrow. The LHS's own
    // use was registered when it was evaluated, which is before the RHS, so
    // nothing kept the borrow live across the invalidation:
    //
    //   int *p = &v[0];
    //   p[0] += v.emplace_back(5);   // emplace_back reallocates, then p[0] is
    //   written
    //
    // The plain-assignment spelling reported this, but only because C++17
    // sequences its RHS before its LHS, putting the LHS use after the
    // invalidation by luck of the ordering. Register the read-modify-write
    // here, at the compound assignment's own program point, so both spellings
    // behave alike.
    if (OriginNode *L = getOriginNode(*BO->getLHS()))
      CurrentBlockFacts.push_back(FactMgr.createFact<UseFact>(BO->getLHS(), L));
    handleUse(BO->getRHS());
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

/// Finds the CFG predecessor of \p MergeBlock that evaluates \p ArmExpr -- i.e.
/// the branch that produces the conditional operator's value on that path.
/// Returns null when no predecessor does, which also covers an arm that cannot
/// produce a value at all (a `throw`, or a call to a `noreturn` function): such
/// an arm has no edge into the merge block.
static const CFGBlock *findPredBlockForExpr(const CFGBlock *MergeBlock,
                                            const Expr *ArmExpr) {
  if (!ArmExpr)
    return nullptr;
  const Expr *Target = ArmExpr->IgnoreParenImpCasts();
  // For the GNU binary conditional `a ?: b`, getTrueExpr() is an
  // OpaqueValueExpr wrapping the common subexpression, and it is that
  // subexpression which appears in the predecessor block.
  if (const auto *OVE = dyn_cast<OpaqueValueExpr>(Target))
    if (const Expr *Src = OVE->getSourceExpr())
      Target = Src->IgnoreParenImpCasts();

  for (const CFGBlock *Pred : MergeBlock->preds()) {
    if (!Pred)
      continue;
    for (const CFGElement &Elt : *Pred)
      if (auto CS = Elt.getAs<CFGStmt>())
        if (const auto *E = dyn_cast<Expr>(CS->getStmt()))
          if (E->IgnoreParenImpCasts() == Target)
            return Pred;
  }
  return nullptr;
}

/// Flows each arm of a conditional operator into its result, generating the flow
/// facts in the arm's own predecessor block rather than in the merge block.
///
/// Emitting both arms' flows in the merge block leaks liveness across a loop
/// backedge: they must then be applied sequentially (one `Kill`, one merge), so
/// going backwards the second arm's origin stays live through the first arm's
/// branch, and around the loop. Path-isolating them lets each arm `Kill`, so
/// `&x` is not live on the `&y` path and vice versa:
///
///   for (int i = 0; i < 2; i++) { int x, y; consume(cond ? &x : &y); }
void FactsGenerator::handleConditionalArms(const Expr &CO, const Expr *TrueExpr,
                                           const Expr *FalseExpr) {
  if (const CFGBlock *TBPred = findPredBlockForExpr(CurrentBlock, TrueExpr))
    flow(getOriginNode(CO), getOriginNode(*TrueExpr), /*Kill=*/true, TBPred);
  if (const CFGBlock *FBPred = findPredBlockForExpr(CurrentBlock, FalseExpr))
    flow(getOriginNode(CO), getOriginNode(*FalseExpr), /*Kill=*/true, FBPred);
}

void FactsGenerator::VisitConditionalOperator(const ConditionalOperator *CO) {
  if (!hasOrigins(CO))
    return;
  handleConditionalArms(*CO, CO->getTrueExpr(), CO->getFalseExpr());
}

void FactsGenerator::VisitBinaryConditionalOperator(
    const BinaryConditionalOperator *BCO) {
  if (!hasOrigins(BCO))
    return;
  // The GNU binary conditional `a ?: b` yields `a` when `a` is truthy, else `b`.
  // Path-isolate the two candidates the same way as the ternary, so neither
  // leaks liveness onto the other's path (see handleConditionalArms). The common
  // subexpression is the "true" value (its OpaqueValueExpr forwards to it in
  // getOrCreateNode).
  handleConditionalArms(*BCO, BCO->getCommon(), BCO->getFalseExpr());
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
  // A C++23 static `operator()` or `operator[]` is still written with object
  // syntax, so the object expression is argument 0 while the callee's parameters
  // start at 0 too. Drop it to avoid an off-by-one that binds the first
  // parameter -- and any [[clang::lifetimebound]] on it -- to the object
  // instead: `r[k]` then claims the result borrows `r`, which outlives it, and
  // the dangle into `k` goes unreported.
  //
  // Ask whether the callee is a static MEMBER rather than naming the operators,
  // so a future static operator needs no change here. `isStatic()` alone would
  // not do: a file-static free operator (`static bool operator==(A, B)`) is
  // static too, and its argument 0 is a real parameter.
  if (const auto *MD = dyn_cast_or_null<CXXMethodDecl>(OCE->getDirectCallee());
      MD && MD->isStatic())
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
  handleAggregateInitOverlap(ILE, Inits);
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
    // (`Box b{...};`) -- but only skip when the declaration really is reported.
    // VisitDeclStmt asks the question of the DECLARED type, so a plain
    // borrow-holding sub-aggregate nested inside a
    // [[gsl::Pointer]]/[[gsl::Owner]] declaration (`View v{Raw{&owner}};`) is
    // reported by neither: the outer type is annotated, and this skip assumed
    // the declaration covered the inner one. The identical sub-aggregate in an
    // escaping temporary (`return View{Raw{p}};`) was reported, so the two
    // spellings disagreed.
    if (const auto *DS = dyn_cast<DeclStmt>(P)) {
      bool DeclIsReported = false;
      auto &Cache = FactMgr.getUnknownOwnershipCache();
      for (const Decl *D : DS->decls())
        if (const auto *VD = dyn_cast<VarDecl>(D)) {
          QualType VDType = VD->getType();
          while (const ArrayType *AT = VDType->getAsArrayTypeUnsafe())
            VDType = AT->getElementType();
          // The same four questions VisitDeclStmt asks, in the same order; any
          // of them reporting means the declaration covers this aggregate.
          bool IsPointer = false;
          if (isUnknownOwnershipType(VDType, Cache) ||
              isGslOwnerOfIndirection(VDType, Cache) ||
              isGslPointerOfIndirection(VDType, Cache) ||
              !findNestedOwnerOrPointerOfIndirection(VDType, Cache, IsPointer)
                   .isNull()) {
            DeclIsReported = true;
            break;
          }
        }
      if (DeclIsReported)
        return;
      break;
    }
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
  // A temporary's storage is a borrow root whatever its duration. Issuing the loan only
  // for a full-expression temporary left a reference bound to a lifetime-extended one
  // holding an EMPTY origin -- so no expiry could ever fire for it, and the `lost-loan`
  // sentinel, which needs the origin to be entirely empty, was masked by any co-resident
  // real loan. A conditional with a tracked borrow in its other arm was enough to hide a
  // use-after-free completely.
  //
  // Where each kind expires: a full-expression temporary at the cleanup the CFG marks
  // (handleFullExprCleanup); one extended by binding to a local reference with that
  // reference's scope (handleLifetimeEnds, which finds it from the initializer); one
  // extended to static or thread duration never, within this function -- its storage
  // outlives every use here, which is exactly what the loan should say.
  const Loan *L = createLoan(FactMgr, MTE);
  CurrentBlockFacts.push_back(
      FactMgr.createFact<IssueFact>(L->getID(), OuterMTEID));
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
  // Subscripting reads the BASE in order to follow it, exactly as a dereference
  // does -- including for a write through the subscript (`p[i] = x`), where the
  // element is overwritten but `p` itself is still read. Registering that read
  // here, at the subscript's own program point, is what puts it after the
  // index: the index can invalidate what the base borrows, and then the base is
  // used anyway.
  //
  //   int *p = new int(7);
  //   sink = p[(delete p, 0)];   // the index frees p, then p is read
  //
  // Nothing modelled a use of the base at all, so the borrow was not live at
  // the deallocation and this went unreported -- while the dereference spelling
  // `*(delete p, p)` was caught.
  //
  // Recorded as an IMPLICIT use. The read is part of the subscript rather than
  // a use the author wrote separately, and the base carries the same loans as
  // the object already being reported, so an explicit use would double-report
  // every ordinary `sv.data()[0]` under the lost-loan and borrow-from-global
  // checks (both of which skip implicit uses). Expiry, invalidation and
  // use-after-free all have an implicit-use reporting path, which is what this
  // needs.
  if (OriginNode *BaseNode =
          getRValueOrigins(ASE->getBase(), getOriginNode(*ASE->getBase())))
    CurrentBlockFacts.push_back(
        FactMgr.createFact<UseFact>(ASE->getExprLoc(), BaseNode));
}

/// Which placement argument the allocation function's result may point into, or
/// -1 when the result is fresh storage.
///
/// Deciding this from the SIGNATURE -- "the parameter after the size is
/// `void*`, so this must be the non-allocating form" -- is a guess about the
/// body, and it guesses wrong both ways. A custom allocation function returning
/// a `char*` buffer it was handed IS placement and was treated as a fresh
/// allocation, so freeing the buffer left the placed object's borrow dangling
/// with nothing reported. An allocation function that takes a `void*` and
/// genuinely allocates
/// (`return ::operator new(n);` -- an allocator wrapper) is NOT placement, and
/// was reported as a use-after-free that ASan says does not exist.
///
/// '[[clang::lifetimebound]]' states exactly the relationship placement needs
/// -- the result may point into this parameter -- so a user-written allocation
/// function is read from its annotation, and the existing lifetimebound body
/// verifier keeps that annotation honest. The library's standard non-allocating
/// form carries no annotation, so it is still recognized by its signature; that
/// is a fixed, known declaration rather than a guess about arbitrary code.
static int placementArgResultPointsInto(const FunctionDecl *OperatorNew) {
  if (!OperatorNew || OperatorNew->getNumParams() <= 1)
    return -1;
  // Parameter 0 is the size; placement parameters follow.
  for (unsigned I = 1, N = OperatorNew->getNumParams(); I != N; ++I)
    if (OperatorNew->getParamDecl(I)->hasAttr<LifetimeBoundAttr>())
      return static_cast<int>(I - 1);
  // The RESERVED global `::operator new(size_t, void*)` -- the standard
  // non-allocating form, which [new.delete.placement] specifies to return its
  // second argument. It carries no annotation, and recognizing it by signature
  // is not a guess: that exact signature is reserved, so it means this wherever
  // it is declared (freestanding code and tests declare it themselves rather
  // than including <new>). Restricted to a non-member with exactly those two
  // parameters -- a class-specific `operator new` taking a `void*`, or a global
  // one with extra parameters, says nothing about whether it returns that
  // pointer and must come from the annotation instead.
  if (!isa<CXXMethodDecl>(OperatorNew) && OperatorNew->getNumParams() == 2 &&
      OperatorNew->getParamDecl(1)->getType()->isVoidPointerType())
    return 0;
  return -1;
}

bool FactsGenerator::handlePlacementNew(const CXXNewExpr *NE,
                                        OriginNode *NewNode,
                                        OriginNode **BufferOut) {
  if (NE->getNumPlacementArgs() < 1)
    return false;
  int Which = placementArgResultPointsInto(NE->getOperatorNew());
  if (Which < 0 || static_cast<unsigned>(Which) >= NE->getNumPlacementArgs())
    return false;

  // Use the placement argument before the implicit conversion to void*, so
  // inner origins are still available.
  const Expr *PlacementArg = NE->getPlacementArg(Which);
  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(PlacementArg);
      ICE && ICE->getCastKind() == CK_BitCast &&
      PlacementArg->getType()->isVoidPointerType())
    PlacementArg = ICE->getSubExpr();
  OriginNode *PlacementNode = getOriginNode(*PlacementArg);
  if (BufferOut)
    *BufferOut = PlacementNode;

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
  OriginNode *PlacementBuffer = nullptr;
  bool IsPlacement = handlePlacementNew(NE, NewNode, &PlacementBuffer);
  if (!IsPlacement && NE->getNumPlacementArgs() != 1) {
    const Loan *L = createLoan(FactMgr, NE);
    CurrentBlockFacts.push_back(
        FactMgr.createFact<IssueFact>(L->getID(), NewNode->getOriginID()));
  }
  // Soundness: the loan above says the result is FRESH storage, which is only
  // true if the allocation function does not hand back a pointer it was given.
  // For a user-written allocation function that is what
  // '[[clang::lifetimebound]]' declares, so an UNANNOTATED pointer/reference
  // placement parameter leaves the question open -- and answering it "fresh"
  // silently drops the borrow, which is exactly the custom-placement bypass
  // with the annotation removed. Route the placement arguments through the same
  // unannotated-indirection check a normal call gets; the operator-new call is
  // modelled here rather than by handleFunctionCall, so nothing else asks.
  if (const FunctionDecl *OperatorNew = NE->getOperatorNew();
      OperatorNew &&
      !OperatorNew->getASTContext().getSourceManager().isInSystemHeader(
          OperatorNew->getLocation())) {
    llvm::SmallVector<const Expr *, 4> PlacementArgs(
        NE->placement_arguments().begin(), NE->placement_arguments().end());
    // The allocation function's parameter 0 is the size, which has no
    // argument here; prepend a null so index i lines up with parameter i.
    // The argument helpers skip a null argument.
    PlacementArgs.insert(PlacementArgs.begin(), nullptr);
    if (!IsPlacement)
      handleUnannotatedIndirectionArgs(OperatorNew, PlacementArgs);
    // The placement argument the result points INTO is the storage being
    // constructed in, not an owner the call may reallocate: `new (buf) T`
    // writes bytes into `buf`, it does not move it. Its parameter is typically
    // `void *`, which paramMayMutateOwner deliberately treats as mutable (an
    // opaque pointee cannot be shown to be owner-free -- the C-interop userdata
    // idiom), so without excluding it every placement-new into a buffer would
    // report an invalidation of that buffer. Blank the slot, as with the size.
    llvm::SmallVector<const Expr *, 4> MutationArgs(PlacementArgs);
    if (int Buf = placementArgResultPointsInto(OperatorNew); Buf >= 0)
      if (static_cast<unsigned>(Buf) + 1 < MutationArgs.size())
        MutationArgs[Buf + 1] = nullptr;
    // A placement argument is an ordinary argument as far as what the callee
    // may do to it: `new (Tag{}, v) T` can reallocate `v` exactly as
    // `::operator new(sizeof(T), Tag{}, v)` can. Spelling the same call as a
    // new-expression routed it here instead of through handleFunctionCall, and
    // only the unannotated-indirection question was re-asked -- so the mutation
    // went unmodelled while both the explicit call and an identically-signed
    // plain function were reported. Ask the argument-side questions here too.
    //
    // Not gated on IsPlacement: whether the operator hands back a pointer into
    // one argument says nothing about what it does to the others.
    handleAssumedInvalidatingCall(NE, OperatorNew, MutationArgs);
    handleArgumentOverlap(NE, OperatorNew, MutationArgs);
    handleMovedArgsInCall(OperatorNew, MutationArgs);
  }

  NewNode = NewNode->getPointeeChild();

  if (!NewNode || !Init)
    return;

  // FIXME: OriginNode is null for `new[]` initializers. Remove this `Init`
  // check once array origins are supported.
  if (OriginNode *InitNode = getOriginNode(*Init); InitNode) {
    flow(NewNode, InitNode, true);
    // A placement new constructs into storage that already exists and outlives
    // the expression, so a borrow the new object captures comes to rest THERE.
    // The flow above only reaches the new-expression's own pointee origin,
    // which for a placement form is a throwaway -- `new (this) S{t}` left the
    // borrow of `t` in it and the object never received it, while both `v = t`
    // and `*this = S{t}` in the same method were reported.
    //
    // Route it by the loans the buffer holds, as any other store through an
    // lvalue is: those name the storage written, so `this` resolves to the
    // object's own origin and a later read of its member sees the borrow.
    if (IsPlacement && PlacementBuffer)
      CurrentBlockFacts.push_back(FactMgr.createFact<DynamicStoreFact>(
          PlacementBuffer->getOriginID(), InitNode->getOriginID(), NE));
  }
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

void FactsGenerator::VisitOMPExecutableDirective(
    const OMPExecutableDirective *D) {
  // An OpenMP directive's data-sharing clauses make copies the analysis does
  // not model -- `private`/`firstprivate` give each thread its own object, so a
  // borrow of the original names storage the body never touches -- and the body
  // may run concurrently, so a borrow's validity no longer follows from the
  // sequential control flow the analysis reasons about. Reject the construct
  // under the safe programming model.
  //
  // The directive's BODY is still walked normally, so a hazard written there is
  // reported precisely as well; this refusal covers what the clauses and the
  // concurrency hide.
  CurrentBlockFacts.push_back(FactMgr.createFact<UntrackedConstructFact>(
      UntrackedConstructReason::OpenMPDirective, D->getBeginLoc()));
}

void FactsGenerator::handleTryStatements() {
  // Shallow worklist over the function's statements. Do not descend into nested
  // lambdas/blocks; their bodies are separate functions analyzed on their own.
  llvm::SmallVector<const Stmt *, 32> Worklist;
  if (const Stmt *Body = AC.getBody())
    Worklist.push_back(Body);
  // A constructor's MEM-INITIALIZERS are not part of its body, so a `try`
  // written there -- inside a statement-expression initializing a member -- is
  // not reachable from the body alone and went unrefused, while the same `try`
  // one line further down in the body was refused. runPreScan already seeds
  // both; do the same here.
  if (const auto *CD = dyn_cast_if_present<CXXConstructorDecl>(AC.getDecl()))
    for (const CXXCtorInitializer *Init : CD->inits())
      if (const Expr *InitE = Init->getInit())
        Worklist.push_back(InitE);
  if (Worklist.empty())
    return;
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

/// A member's destructor runs *after* the enclosing destructor's body, and it can
/// read whatever that body stored into the member. The analysis is
/// intra-procedural and does not see the member destructor's body, so model the
/// member's destruction as a *use* of the member -- exactly as handleLifetimeEnds
/// does for a local object at scope exit. This keeps a borrow the body deposited
/// in the member live up to that point, so a local that died at the end of the
/// body is reported as not living long enough.
///
/// Owners are excluded (their destruction frees their own storage, already modeled
/// by expiry, rather than dereferencing a borrow into something else), and a
/// trivial destructor cannot read anything.
void FactsGenerator::handleMemberDtor(const CFGMemberDtor &MemberDtor) {
  const FieldDecl *FD = MemberDtor.getFieldDecl();
  if (!FD)
    return;
  // A member is destroyed by its enclosing object's destructor, and its own destructor can
  // read or mutate what it captured just as a local's can, so this is the same model. The
  // member case is reached only while analyzing the enclosing destructor, which is why a
  // guard held as a member of an object with no user-written destructor is still missed --
  // see the sibling-subobject case, which needs member origins seeded there.
  //
  // A member's destruction has no expression of its own -- CFGMemberDtor carries only the
  // field -- so the enclosing destructor's body is what a report can point at.
  if (OriginNode *Node = getOriginNode(*FD))
    handleDestructionOfBorrowHolder(FD->getType(), Node, AC.getBody(),
                                    AC.getDecl()->getEndLoc());
}

/// Collects the fields \p RD declares, and those its own bases declare.
static void
collectFieldsIncludingBases(const CXXRecordDecl *RD,
                            llvm::SmallPtrSetImpl<const CXXRecordDecl *> &Seen,
                            llvm::SmallVectorImpl<const FieldDecl *> &Out) {
  if (!RD || !RD->hasDefinition() ||
      !Seen.insert(RD->getCanonicalDecl()).second)
    return;
  for (const CXXBaseSpecifier &B : RD->bases())
    collectFieldsIncludingBases(B.getType()->getAsCXXRecordDecl(), Seen, Out);
  llvm::append_range(Out, RD->fields());
}

void FactsGenerator::handleBaseDtor(const CFGBaseDtor &BaseDtor) {
  const CXXBaseSpecifier *BS = BaseDtor.getBaseSpecifier();
  if (!BS)
    return;
  // A BASE subobject outlives the derived destructor's BODY: its destructor
  // runs after the body returns, and it can read the base's own members. So a
  // borrow stored into an inherited member during the body is still held here
  // -- which is why "a field does not outlive a destructor" (see
  // handleExitBlock) is not the whole story, and why dropping this element lost
  // the hazard entirely.
  //
  // The origin model has no node for a base subobject, but the base's FIELDS
  // have origins, and those are exactly what its destructor can reach. Use
  // them, so a local whose borrow was stored into an inherited member is still
  // live here and its expiry is reported.
  llvm::SmallPtrSet<const CXXRecordDecl *, 4> Seen;
  llvm::SmallVector<const FieldDecl *, 4> Fields;
  collectFieldsIncludingBases(BS->getType()->getAsCXXRecordDecl(), Seen,
                              Fields);
  for (const FieldDecl *FD : Fields)
    if (OriginNode *Node = getOriginNode(*FD))
      CurrentBlockFacts.push_back(
          FactMgr.createFact<UseFact>(AC.getDecl()->getEndLoc(), Node));
}

/// Returns true if `PT` is, or holds, a TYPE-ERASED callable (std::function and
/// friends). What such a callable captured is invisible: a by-reference capture
/// of an owner is erased behind the wrapper's interface, so invoking it --
/// which is what a guard's destructor does -- may reallocate storage the caller
/// holds a borrow into, with nothing in the type saying so.
///
/// The other callable shapes are covered elsewhere: a lambda held directly
/// exposes its captures as fields, a function pointer is refused as an indirect
/// call, and a capturing lambda in a plain struct is refused as
/// unknown-ownership. A type-erased wrapper inside an ANNOTATED record had
/// neither -- the annotation says the record is modeled, and the wrapper hides
/// the capture.
static bool holdsTypeErasedCallable(QualType PT) {
  const CXXRecordDecl *RD = PT.getNonReferenceType()->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return false;
  if (isStdCallableWrapperType(RD))
    return true;
  for (const FieldDecl *F : RD->fields()) {
    const CXXRecordDecl *FRD =
        arrayElementType(F->getType())->getAsCXXRecordDecl();
    if (FRD && FRD->hasDefinition() && isStdCallableWrapperType(FRD))
      return true;
  }
  return false;
}

/// Models the destruction of an object that may hold a borrow.
///
/// Shared by every way an object can be destroyed, because the hazard does not
/// depend on how: a scope ending, a temporary's full-expression cleanup, or an
/// enclosing object's destructor running for a member. Modelling it only for a
/// named local meant an unnamed guard -- `(void)Grower{&v}.vec;` -- and a guard
/// held as a MEMBER of another object were both silent, while the
/// byte-identical named local was reported.
///
/// A non-trivial destructor may READ a borrow the object holds (a
/// [[gsl::Pointer]] whose
/// `~T()` dereferences its captured view), and the analysis is intra-procedural
/// and cannot see that body -- so model the destruction as a use, keeping the
/// borrow live to this point. Owners are excluded: destroying one frees its own
/// storage, which the expiry already models, rather than dereferencing a borrow
/// into another object.
///
/// It may also MUTATE the owner it captured -- `~Trigger() { o->grow(); }`
/// reallocates -- which the analysis equally cannot see. Treat that as an
/// assumed invalidation of the borrows the object carries on its captured
/// owner, so a view into that owner living past the guard is reported while one
/// used only during the guard's lifetime is not. Gated on paramMayMutateOwner,
/// the same test used for call arguments, so a guard aliasing only a const
/// owner is not flagged. A gsl::Pointer is a leaf in the origin tree, so the
/// captured borrow sits on the object's own origin; invalidate that and its
/// whole pointee chain (the latter for a nested wrapper reaching the owner
/// through a by-value gsl::Pointer member). Returns true if destroying an
/// object of type `PT` runs a destructor that may reallocate an owner the
/// object only ALIASES, so borrows into that owner die with it.
/// `paramMayMutateOwner` answers this for a pointer/reference and for a
/// gsl::Pointer wrapper; a LAMBDA closure is neither, yet destroying one runs
/// every capture's destructor -- so an init-capture of a guard
/// (`[g = Grower{&v}]{}`) reallocates the borrowed owner when the closure dies.
/// A closure is exempt from the unknown-ownership ban (a lambda value is
/// modeled directly), and it carries no annotation, so nothing else covered it
/// while the same guard held by an annotated wrapper or a plain struct was
/// reported.
///
/// Asking `paramMayMutateOwner` of each capture is what keeps a by-value
/// capture of an OWNER silent: that capture is a copy, and destroying a copy
/// invalidates no borrow of the original.
static bool destructionMayMutateAliasedOwner(QualType PT) {
  if (paramMayMutateOwner(PT) || holdsTypeErasedCallable(PT))
    return true;
  const CXXRecordDecl *RD = PT->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition() || !RD->isLambda())
    return false;
  for (const FieldDecl *F : RD->fields())
    if (paramMayMutateOwner(arrayElementType(F->getType())))
      return true;
  return false;
}

void FactsGenerator::handleDestructionOfBorrowHolder(QualType Ty,
                                                     OriginNode *Node,
                                                     const Stmt *Trigger,
                                                     SourceLocation Loc) {
  // Peel array dimensions. An array of guards destroys every element, and the
  // origin tree already shares one origin across the elements -- so the element
  // type is what decides the hazard, exactly as for a single guard. Without this
  // the array type has no CXXRecordDecl and the whole check bailed out, so
  // `Grower arr[1] = {Grower{&v}};` destroyed a guard with the caller's borrows
  // live and reported nothing, while the scalar `Grower g{&v};` reported.
  Ty = arrayElementType(Ty);
  const CXXRecordDecl *RD = Ty->getAsCXXRecordDecl();
  // Destroying an OWNER frees what it owns, which is its job rather than an
  // aliasing hazard -- unless it holds a type-erased callable, whose captures
  // are invisible and may reference storage the caller borrows.
  if (!RD || !RD->hasDefinition() || !RD->hasNonTrivialDestructor() ||
      (isGslOwnerType(Ty) && !holdsTypeErasedCallable(Ty)) || !hasOrigins(Ty))
    return;
  CurrentBlockFacts.push_back(FactMgr.createFact<UseFact>(Loc, Node));
  if (!destructionMayMutateAliasedOwner(Ty))
    return;
  auto invalidate = [&](OriginID OID) {
    CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
        OID, Trigger, /*Assumed=*/true));
  };
  invalidate(Node->getOriginID());
  for (OriginNode *Pointee = Node->getPointeeChild(); Pointee;
       Pointee = Pointee->getPointeeChild())
    invalidate(Pointee->getOriginID());
}

/// Collects the temporaries whose lifetime is extended to \p VD's scope.
///
/// Derived from the initializer rather than remembered when the temporary was visited, so
/// it does not depend on the order the CFG's blocks happen to be walked in.
static void collectExtendedTemporaries(
    const VarDecl *VD,
    llvm::SmallVectorImpl<const MaterializeTemporaryExpr *> &Out) {
  const Expr *Init = VD->getInit();
  if (!Init)
    return;
  llvm::SmallVector<const Stmt *, 8> Worklist{Init};
  while (!Worklist.empty()) {
    const Stmt *S = Worklist.pop_back_val();
    if (!S)
      continue;
    if (const auto *MTE = dyn_cast<MaterializeTemporaryExpr>(S))
      if (MTE->getStorageDuration() == SD_Automatic) {
        // Extended to this variable, or to one of its subobjects. Clang records
        // the target as the variable for a temporary bound one level down, but
        // leaves it as the FIELD once another aggregate is nested in between --
        // and either way the storage dies with this variable, because the
        // object owning that member is this variable. A temporary belonging to
        // some OTHER variable (one declared inside a lambda body in this
        // initializer, say) names that variable and is left to its own scope.
        const ValueDecl *Extended = MTE->getExtendingDecl();
        if (Extended == VD || isa_and_nonnull<FieldDecl>(Extended))
          Out.push_back(MTE);
      }
    // A default initializer holds its expression on the FIELD, so
    // CXXDefaultInitExpr::children() is an empty range and walking children
    // alone never enters it. A temporary bound to a reference member by a
    // default member initializer is extended to the enclosing OBJECT, so it
    // dies with this variable -- but it lives behind exactly that empty range,
    // so it was never collected and never expired, leaving the borrow immortal.
    // Same for a default argument.
    if (const auto *DIE = dyn_cast<CXXDefaultInitExpr>(S))
      Worklist.push_back(DIE->getExpr());
    else if (const auto *DAE = dyn_cast<CXXDefaultArgExpr>(S))
      Worklist.push_back(DAE->getExpr());
    for (const Stmt *Child : S->children())
      Worklist.push_back(Child);
  }
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
  if (OriginNode *Node = getOriginNode(*LifetimeEndsVD))
    handleDestructionOfBorrowHolder(VDTy, Node, LifetimeEnds.getTriggerStmt(),
                                    LifetimeEnds.getTriggerStmt()->getEndLoc());
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
  // A temporary lifetime-extended by binding to this variable dies here too, and its
  // storage is a borrow root in its own right -- the variable's own AccessPath above
  // describes the reference, not the object it was bound to.
  llvm::SmallVector<const MaterializeTemporaryExpr *, 2> Extended;
  collectExtendedTemporaries(LifetimeEndsVD, Extended);
  for (const MaterializeTemporaryExpr *MTE : Extended)
    CurrentBlockFacts.push_back(FactMgr.createFact<ExpireFact>(
        AccessPath(MTE), LifetimeEnds.getTriggerStmt()->getEndLoc()));
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

void FactsGenerator::handleTemporaryDtor(
    const CFGTemporaryDtor &TemporaryDtor) {
  // A temporary whose value is DISCARDED is never materialized, so no
  // MaterializeTemporaryExpr exists for it and handleFullExprCleanup below
  // never sees it -- the CFG marks its destruction with a CFGTemporaryDtor
  // instead. Its destructor still runs and can mutate what the object borrowed,
  // so `Grower{&v};` left the caller's live borrows silently invalidated while
  // every form that names the temporary or reads a member of it was reported.
  const CXXBindTemporaryExpr *BTE = TemporaryDtor.getBindTemporaryExpr();
  if (!BTE)
    return;
  // Which temporaries are this handler's to model is decided by asking who ELSE
  // already models this one, and modelling it otherwise. Enumerating the
  // contexts that DISCARD a value was the fragile part: an unrecognized parent
  // was assumed to consume the value, so every missing entry -- a comma, a
  // conditional,
  // `__builtin_choose_expr`, `_Generic` -- was a silent miss. With the default
  // inverted, a missing entry costs a duplicate diagnostic instead of a dropped
  // one, which is the right way round for a soundness check.
  //
  // Climb the wrappers whose operand shares their own fate: parens, casts and
  // the full-expression cleanup forward the value; a comma discards its left
  // operand; a conditional, `__builtin_choose_expr` and `_Generic` SELECT an
  // arm rather than consume it. Whatever stops the climb is what consumes the
  // temporary.
  auto SharesOperandFate = [](const Stmt *P) {
    if (isa<ParenExpr>(P) || isa<CastExpr>(P) || isa<ExprWithCleanups>(P) ||
        isa<AbstractConditionalOperator>(P) || isa<ChooseExpr>(P) ||
        isa<GenericSelectionExpr>(P))
      return true;
    const auto *BO = dyn_cast<BinaryOperator>(P);
    return BO && BO->getOpcode() == BO_Comma;
  };
  const ParentMap &PM = AC.getParentMap();
  const Stmt *Consumer = PM.getParent(BTE);
  while (isa_and_present<Expr>(Consumer) && SharesOperandFate(Consumer))
    Consumer = PM.getParent(Consumer);
  // A MATERIALIZED temporary is destroyed at the cleanup handleFullExprCleanup
  // sees, or at the extending variable's scope exit. That is a property of the
  // temporary rather than of its syntactic position.
  if (isa_and_present<MaterializeTemporaryExpr>(Consumer))
    return;
  // A temporary handed to a CALL is modelled where the call consumes it: a
  // mutating gsl::Pointer argument already yields an assumed invalidation
  // there, so modelling the destructor here too would report one hazard twice.
  if (isa_and_present<CallExpr>(Consumer) ||
      isa_and_present<CXXConstructExpr>(Consumer))
    return;
  if (OriginNode *Node = getOriginNode(*BTE))
    handleDestructionOfBorrowHolder(BTE->getSubExpr()->getType(), Node, BTE,
                                    BTE->getEndLoc());
}

void FactsGenerator::handleFullExprCleanup(
    const CFGFullExprCleanup &FullExprCleanup) {
  for (const auto *MTE : FullExprCleanup.getExpiringMTEs()) {
    // A temporary's destructor runs here and can read or mutate what the object captured,
    // exactly as a named local's can at scope exit. Emitting only the expiry left an
    // unnamed RAII guard -- `(void)Grower{&v}.vec;` -- silent, while naming it reported.
    if (OriginNode *Node = getOriginNode(*MTE))
      handleDestructionOfBorrowHolder(MTE->getSubExpr()->getType(), Node, MTE,
                                      FullExprCleanup.getCleanupLoc());
    CurrentBlockFacts.push_back(FactMgr.createFact<ExpireFact>(
        AccessPath(MTE), FullExprCleanup.getCleanupLoc()));
  }
}

void FactsGenerator::handleExitBlock() {
  // A field does not outlive a DESTRUCTOR: by the time one returns the object is
  // gone, so nothing can read its members afterwards and "this borrow escapes to
  // a field" is vacuous there. Emitting the fact anyway makes a destructor's own
  // cleanup (`~Box() { delete pv; }`) look like it strands a borrow in `pv`, and
  // keeps every field origin spuriously live back through the destructor body.
  // Globals still escape from a destructor, so only the field facts are skipped.
  const bool InDestructor = isa<CXXDestructorDecl>(AC.getDecl());
  for (const Origin &O : FactMgr.getOriginMgr().getOrigins())
    if (auto *FD = dyn_cast_if_present<FieldDecl>(O.getDecl())) {
      // Create FieldEscapeFacts for all field origins that remain live at exit.
      if (!InDestructor)
        EscapesInCurrentBlock.push_back(
            FactMgr.createFact<FieldEscapeFact>(O.ID, FD));
    } else if (auto *VD = dyn_cast_if_present<VarDecl>(O.getDecl())) {
      // Create GlobalEscapeFacts for all origins with global-storage that
      // remain live at exit.
      if (VD->hasGlobalStorage()) {
        EscapesInCurrentBlock.push_back(
            FactMgr.createFact<GlobalEscapeFact>(O.ID, VD));
      }
    }

  // When the analyzed "body" is the INITIALIZER of a variable with static storage
  // duration, whatever borrow it computed comes to rest in that variable -- which
  // outlives the initializer by definition. Nothing else marks this: the borrow
  // lives on the initializer expression's origin, not on any declaration's, so the
  // loop above does not see it, and there is no use to anchor it either.
  //
  // Without this a borrow CAPTURED by a constant initializer is invisible, and
  // that is precisely the static destruction-order hazard: `Reader r{v};` stores a
  // reference to another static object, and `~Reader` reads it after `v` is gone.
  if (const auto *VD = dyn_cast_if_present<VarDecl>(AC.getDecl()))
    if (VD->hasGlobalStorage() && VD->getInit())
      if (OriginNode *Init = getOriginNode(*VD->getInit()))
        EscapesInCurrentBlock.push_back(
            FactMgr.createFact<GlobalEscapeFact>(Init->getOriginID(), VD));

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
  if (auto ThisOrigins = FactMgr.getOriginMgr().getThisOrigins()) {
    CurrentBlockFacts.push_back(FactMgr.createFact<UseFact>(
        AC.getDecl()->getEndLoc(), *ThisOrigins));
    // The object outlives the call, so a borrow resting in it at exit escapes
    // -- however it got there. Only a store into a NAMED member produced an
    // escape fact, so a capture that lands on the object instead (a
    // whole-object assignment, a placement new, a helper declared
    // lifetime_capture_by(this), an inherited setter) was invisible to the
    // annotation checks, and a [[clang::lifetimebound]] parameter could be
    // captured into the object with nothing said.
    EscapesInCurrentBlock.push_back(
        FactMgr.createFact<ObjectEscapeFact>((*ThisOrigins)->getOriginID()));
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
    if (!Args[I] || !PVD->getType()->isRValueReferenceType())
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
        FD && isMutableOwnerType(FD->getType()) &&
        // A *reference* member is an alias, not storage of its own: the receiver's
        // loan names the referent, not the field. Scoping the invalidation to the
        // field would then match nothing (and the early return in
        // IsExactInvalidated also skips the generic access-path comparison that
        // does match), so `c.items.clear()` through a `vector<T> &items` member
        // invalidated nothing -- while the pointer spelling of the same design,
        // whose receiver is a dereference rather than a MemberExpr, worked.
        !FD->getType()->isReferenceType())
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

  // The origin of the call's own result, if it has one. A borrow the call returns
  // is taken after whatever the call does, so this same call cannot have
  // invalidated it -- see InvalidateOriginFact::getResultOrigin.
  std::optional<OriginID> ResultOrigin;
  if (hasOrigins(Call))
    if (OriginNode *CallNode = getOriginNode(*Call))
      ResultOrigin = CallNode->getOriginID();
  auto invalidate = [&](OriginID OID,
                        OwnerLoanGate LoanGate = OwnerLoanGate::None) {
    CurrentBlockFacts.push_back(FactMgr.createFact<InvalidateOriginFact>(
        OID, Call, /*Assumed=*/true, /*Deallocation=*/false,
        /*MutatedField=*/nullptr, LoanGate, ResultOrigin));
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
    // through a pointer-to-owner is spuriously treated as a mutation. The
    // name-based allow-list only describes std types, so it is gated on the
    // receiver being an owner; an explicit
    // `[[clang::lifetime_non_invalidating]]` promise is not, since it also covers
    // an accessor of a record that merely *contains* owners.
    // The name-based allow-list only describes std types, so it is gated on the
    // receiver being a std owner -- or a std VIEW: iterator traversal
    // (`++it`) has a view receiver, mutates only the iterator, and cannot
    // reallocate the container the iterator points into. Without admitting
    // views here a plain read-only loop over a member container looks like a
    // mutation of the enclosing object, since the iterator carries that
    // object's loan.
    bool PointeeIsOwner = isGslOwnerType(RecvRecordTy);
    bool PointeeIsView = isGslPointerType(RecvRecordTy);
    bool NonInvalidating =
        Method->hasAttr<LifetimeNonInvalidatingAttr>() ||
        ((PointeeIsOwner || PointeeIsView) && isNonInvalidatingMethod(*Method));
    if ((StaticallyOwner || RecvRD) && !NonInvalidating)
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
    // A new-expression has no argument for the allocation function's size
    // parameter, so that slot is null to keep the indices aligned.
    if (!Args[I])
      continue;
    const ParmVarDecl *PVD = paramForArg(FD, IsInstance, I);
    if (!PVD)
      continue;
    // The parameter's *static* type does not always reveal an owner the call may
    // mutate: passing `*this` to a parameter typed as a base erases the
    // reachability edge, and the callee reaches the derived object again through a
    // virtual call or a plain `static_cast`. Nor does the argument's static type
    // answer it -- the upcast may have happened earlier (`Base &b = derived;
    // f(b);`). So the parameter decides only MUTABILITY (can the callee write
    // through it at all), and reachability is confirmed from the loans the argument
    // actually carries:
    //  - If the parameter's type itself shows a mutable owner, emit unconditionally.
    //  - Otherwise, for any non-const pointer/reference to a class, still emit --
    //    gated (OwnerLoanGate::DenotedOwner) so the checker acts only when a loan
    //    the argument carries denotes an object that is-a the parameter's type and
    //    is (or contains) a mutable owner. This is the same loan-based confirmation
    //    the receiver branch uses; an argument denoting no owner yields nothing.
    bool MayMutate = paramMayMutateOwner(PVD->getType());
    bool LoanConfirmed =
        !MayMutate && paramCanMutateThrough(PVD->getType()) &&
        PVD->getType()->getPointeeType()->getAsCXXRecordDecl() != nullptr;
    if (!MayMutate && !LoanConfirmed)
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
    if (!PVD)
      return false;
    if (paramMayMutateOwner(PVD->getType()))
      return true;
    // The parameter's STATIC type does not always reveal the owner the callee
    // can reallocate. Upcasting the argument to an abstract interface erases it
    // -- `IResettable` declares no data members, so nothing looks mutable,
    // while a virtual call on it dispatches into the derived object and
    // reallocates a std::string there. Passing the derived type instead was
    // reported, so one upcast silenced the same bug.
    //
    // So any non-const pointer/reference to a class counts here. That costs
    // nothing in precision: this only decides whether to EMIT the overlap fact,
    // and the checker confirms the hazard from the loans the two arguments
    // actually carry, reporting only when they genuinely alias. This mirrors
    // the loan-confirmed arm the assumed-invalidation path already uses for
    // exactly this shape (OwnerLoanGate::DenotedOwner).
    return paramCanMutateThrough(PVD->getType()) &&
           PVD->getType()->getPointeeType()->getAsCXXRecordDecl() != nullptr;
  };
  emitArgumentOverlap(Call, Args, IsMutatingArg);
}

/// Assembling an AGGREGATE brings several borrows together at one point exactly as a call
/// does, so the same exclusivity question applies: one initializer that can mutate an owner
/// alongside another that borrows into it must not be combined. Asking it only at calls left
/// `Session s{Token{buf}, Trailer{&buf}}` silent while the identical arguments to a
/// constructor or a free function were both reported -- and a mutating sibling could be
/// assembled next to a borrowing one with nothing said.
///
/// Only what each argument is BOUND to differs: a call's binds to a parameter, an
/// initializer to a field. Everything that follows from that -- which co-arguments carry an
/// aliasing borrow, the pointee chain, the record being mutated -- is identical, so the two
/// share it and supply only this predicate.
void FactsGenerator::handleAggregateInitOverlap(
    const Expr *AggExpr, ArrayRef<const Expr *> Inits) {
  const CXXRecordDecl *RD = AggExpr->getType()->getAsCXXRecordDecl();
  // A union initializes one member, so there are no siblings to overlap with.
  if (!RD || RD->isUnion())
    return;
  // The initializers are in subobject order -- bases first, then fields -- so skip past the
  // bases to align the two, mirroring handleGslAggregateInit. A base initializer has no
  // field and cannot be the mutating one.
  llvm::SmallVector<const FieldDecl *, 4> InitFields(RD->getNumBases(), nullptr);
  llvm::append_range(InitFields, RD->fields());
  auto IsMutatingInit = [&](unsigned I) -> bool {
    return I < InitFields.size() && InitFields[I] &&
           paramMayMutateOwner(InitFields[I]->getType());
  };
  emitArgumentOverlap(AggExpr, Inits, IsMutatingInit);
}

/// Pairs each argument that may mutate an owner with every other argument holding a borrow,
/// and records one fact per mutating argument; the checker reports a pair when their loans
/// actually alias.
void FactsGenerator::emitArgumentOverlap(
    const Expr *At, ArrayRef<const Expr *> Args,
    llvm::function_ref<bool(unsigned)> IsMutatingArg) {
  for (unsigned M = 0; M < Args.size(); ++M) {
    if (!Args[M] || !IsMutatingArg(M))
      continue;
    OriginNode *MutNode = getOriginNode(*Args[M]);
    if (!MutNode)
      continue;
    // Collect the other arguments that could alias the mutated owner into a
    // single fact for it (rather than one fact per pair).
    llvm::SmallVector<OriginID, 4> Borrows;
    for (unsigned B = 0; B < Args.size(); ++B) {
      if (B == M || !Args[B])
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
        At, FactMgr.copyToFactStorage(llvm::ArrayRef<OriginID>(MutOrigins)),
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
      // Route the capture by the loans the capturer's LVALUE holds. Those name
      // the object that will hold the borrow, whatever expression designated it
      // -- so an inherited method's receiver, which arrives as a derived-to-base
      // conversion whose own origin is a disconnected copy, deposits into the
      // object rather than into the copy. Additive: the flow below is unchanged,
      // and merge semantics make a duplicate deposit into the same origin
      // harmless. Routing-only, so an unresolvable capturer is not refused --
      // that would invent a diagnostic this path never emitted.
      //
      // Emitted BEFORE the flow, so the destination's PRE-STORE loans are
      // genuinely pre-store. The flow merges the payload into the destination
      // origin, so a checker asking which object the store lands in would
      // otherwise find the payload's own parameter among the destination's loans
      // and mistake the store for a self-store into that parameter.
      if (CapturingOriginNode)
        CurrentBlockFacts.push_back(FactMgr.createFact<DynamicStoreFact>(
            CapturingOriginNode->getOriginID(),
            CapturedOriginNode->getOriginID(), Args[I]));
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
  //
  // An argument bound to a reference or pointer parameter leaves the callee
  // aliasing the object, so the borrow of the object ITSELF is what is used --
  // not the value read out of it. A copy or move constructor (or assignment) is
  // excluded: it also binds a reference, but produces an independent object, and
  // `return g;` reaches this path through exactly that binding.
  // An argument bound to a reference or pointer parameter leaves the callee
  // aliasing the object, so the borrow of the object ITSELF is what is used --
  // not the value read out of it.
  //
  // A copy or move constructor or assignment is excluded: it binds a reference
  // too, and at the call the two are indistinguishable by type (`const Str &`
  // either way), so the callee's KIND is the only local discriminator. `Str c =
  // g;`, `return g;`, `a = g;` and passing `g` to a by-value parameter all reach
  // this path through exactly that binding, and none of them retains a borrow.
  // Generalizing from the special members to "a reference parameter of the
  // callee's own class" would also exempt a user-written `assign(const Str &)`,
  // whose body can hold the very borrow this is meant to catch.
  bool CalleeCopies = false;
  bool ArgsIncludeObject = false;
  if (const auto *Ctor = dyn_cast<CXXConstructorDecl>(FD)) {
    CalleeCopies = Ctor->isCopyOrMoveConstructor();
  } else if (const auto *MD = dyn_cast<CXXMethodDecl>(FD)) {
    CalleeCopies =
        MD->isCopyAssignmentOperator() || MD->isMoveAssignmentOperator();
    ArgsIncludeObject = MD->isInstance();
  }
  for (unsigned I = 0, E = Args.size(); I != E; ++I) {
    const ParmVarDecl *PVD =
        CalleeCopies ? nullptr : paramForArg(FD, ArgsIncludeObject, I);
    bool BoundToReference = PVD && (PVD->getType()->isReferenceType() ||
                                    PVD->getType()->isPointerType());
    handleUse(Args[I], BoundToReference);
  }
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
  // Emit the assumed invalidation AFTER the flows that carry this call's own
  // result out (lifetimebound parameter->return, accessor results, ...).
  // Liveness runs backwards, so with the invalidation emitted first the result
  // flow is processed first and propagates the result's liveness back onto the
  // RECEIVER, which then looks live at the call -- and the call appears to
  // invalidate the borrow it just produced. Emitting it last means the live
  // origin at that point is the result itself, which
  // InvalidateOriginFact::getResultOrigin lets the checker exclude. A borrow that
  // existed *before* the call is live there either way, so real invalidations are
  // unaffected. Deferring to scope exit keeps this correct on every early-return
  // path below.
  auto EmitAssumedInvalidation = llvm::make_scope_exit(
      [&] { handleAssumedInvalidatingCall(Call, FD, Args); });
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
      // The result borrows *somewhere inside* the argument: [[lifetimebound]]
      // promises no more than that, not that it borrows the argument itself and
      // not which subobject. Record that as an Interior (`.*`) step so a later
      // member access cannot turn the imprecision into a precise claim --
      // `o.get().a` must become `o.*.a`, not `o.a`, which would name storage
      // that need not exist (`a` is a field of `o.in`) and would then look
      // provably disjoint from the real borrow `o.in.a`.
      //
      // Only when the result is a BORROW. `.*` describes which subobject a
      // borrow points into, so it is meaningless on a result that owns its
      // value: a constructor initializes the object being built, and a factory
      // like `make_unique<T>(tmp)` returns an owner, not a view into `tmp`.
      // Marking those changes the loan's identity for no benefit, and anything
      // keyed on loans (moved loans, diagnostic anchors) then fails to relate it
      // to the argument's own loan.
      // ...and only when the borrowed thing HAS subobjects to be imprecise
      // about. A borrow of a scalar (`int *choose(int*, int*)`) cannot later be
      // refined by a member access, so `.*` would add nothing while changing the
      // loan's identity -- which everything keyed on loans (moved loans,
      // origin-flow chains, diagnostic anchors) then fails to match.
      QualType RetTy = Call->getType();
      QualType Pointee = RetTy->isPointerType() || RetTy->isReferenceType()
                             ? RetTy->getPointeeType()
                             : RetTy.getNonReferenceType();
      bool ResultIsBorrow = Call->isGLValue() || RetTy->isPointerType() ||
                            RetTy->isReferenceType() ||
                            isGslPointerType(RetTy.getNonReferenceType());
      bool BorrowsARecord =
          Pointee->getAsCXXRecordDecl() || isGslPointerType(Pointee);
      if (ResultIsBorrow && BorrowsARecord && !isa<CXXConstructorDecl>(FD))
        CurrentBlockFacts.push_back(FactMgr.createFact<ProjectionFact>(
            CallNode->getOriginID(), PathElement::getInterior(), Call));
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

void FactsGenerator::handleUse(const Expr *E, bool BoundToReference) {
  OriginNode *Node = getOriginNode(*E);
  if (!Node)
    return;
  // For DeclRefExpr: Remove the outer layer of origin which borrows from the
  // decl directly (e.g., when this is not a reference). This is a use of the
  // underlying decl. A static data member reached as `obj.member` denotes that
  // same variable (getOrCreateNode shares its origins), so it peels alike --
  // otherwise reading a scalar one (`r.plain`) would register a use of its
  // storage origin, which holds no loan, and report a spurious lost loan where
  // the qualified spelling `R::plain` is correctly silent.
  //
  // Not when the value is bound to a reference parameter: there the outer origin
  // -- the borrow of the object itself -- is exactly what the callee receives, and
  // for an owner lvalue there is no r-value origin to peel to at all, so peeling
  // would drop the use entirely.
  const ValueDecl *UsedDecl = nullptr;
  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    UsedDecl = DRE->getDecl();
  else if (auto *ME = dyn_cast<MemberExpr>(E))
    if (auto *Var = dyn_cast<VarDecl>(ME->getMemberDecl());
        Var && Var->hasGlobalStorage())
      UsedDecl = Var;
  if (UsedDecl && !BoundToReference && !UsedDecl->getType()->isReferenceType())
    Node = getRValueOrigins(E, Node);
  // Skip if there is no inner origin (e.g., when it is not a pointer type).
  if (!Node)
    return;
  if (!UseFacts.contains(E)) {
    UseFact *UF = FactMgr.createFact<UseFact>(E, Node);
    if (BoundToReference)
      UF->markAsReferenceBinding();
    CurrentBlockFacts.push_back(UF);
    UseFacts[E] = UF;
  } else if (BoundToReference) {
    UseFacts[E]->markAsReferenceBinding();
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
