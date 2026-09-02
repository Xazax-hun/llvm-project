//===- Origins.cpp - Origin Implementation -----------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/Analyses/LifetimeSafety/Origins.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TypeBase.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeStats.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Loans.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "llvm/ADT/StringMap.h"

namespace clang::lifetimes::internal {
namespace {
/// A utility class to traverse the function body in the analysis
/// context and collect the count of expressions with missing origins.
class MissingOriginCollector
    : public RecursiveASTVisitor<MissingOriginCollector> {
public:
  MissingOriginCollector(
      const llvm::DenseMap<const clang::Expr *, OriginNode *> &ExprToOriginNode,
      const OriginManager &OM, LifetimeSafetyStats &LSStats)
      : ExprToOriginNode(ExprToOriginNode), OM(OM), LSStats(LSStats) {}
  bool VisitExpr(Expr *E) {
    if (!OM.hasOrigins(E))
      return true;
    // Check if we have an origin for this expression.
    if (!ExprToOriginNode.contains(E)) {
      // No origin found: count this as missing origin.
      LSStats.ExprTypeToMissingOriginCount[E->getType().getTypePtr()]++;
      LSStats.ExprStmtClassToMissingOriginCount[std::string(
          E->getStmtClassName())]++;
    }
    return true;
  }

private:
  const llvm::DenseMap<const clang::Expr *, OriginNode *> &ExprToOriginNode;
  const OriginManager &OM;
  LifetimeSafetyStats &LSStats;
};

class PreScanCollector : public RecursiveASTVisitor<PreScanCollector> {
public:
  bool VisitCallExpr(const CallExpr *CE) {
    // Indirect calls (e.g., function pointers) are skipped because lifetime
    // annotations currently apply to declarations, not types.
    if (const auto *FD = CE->getDirectCallee())
      collect(FD, FD->getReturnType());
    return true;
  }

  bool VisitCXXConstructExpr(const CXXConstructExpr *CCE) {
    collect(CCE->getConstructor(), CCE->getType());
    return true;
  }

  bool VisitMemberExpr(const MemberExpr *ME) {
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl()))
      AccessedFields.insert(FD);
    return true;
  }

  bool shouldVisitLambdaBody() const { return false; }
  bool shouldVisitTemplateInstantiations() const { return true; }

  const llvm::SmallVector<QualType> &getCollectedTypes() const {
    return CollectedTypes;
  }

  const llvm::SmallPtrSet<const FieldDecl *, 8> &getAccessedFields() const {
    return AccessedFields;
  }

private:
  llvm::SmallVector<QualType> CollectedTypes;
  llvm::SmallPtrSet<const FieldDecl *, 8> AccessedFields;

  void collect(const FunctionDecl *FD, QualType RetType) {
    if (!FD)
      return;
    FD = getDeclWithMergedLifetimeBoundAttrs(FD);

    if (const auto *MD = dyn_cast<CXXMethodDecl>(FD);
        MD && MD->isInstance() && !isa<CXXConstructorDecl>(MD) &&
        implicitObjectParamIsLifetimeBound(MD)) {
      CollectedTypes.push_back(RetType);
      return;
    }

    for (const auto *Param : FD->parameters()) {
      if (Param->hasAttr<LifetimeBoundAttr>()) {
        CollectedTypes.push_back(RetType);
        return;
      }
    }
  }
};

} // namespace

bool OriginManager::hasOrigins(QualType QT) const {
  // An `_Atomic(T)` wraps T transparently for lifetime purposes (the atomic
  // holds the same value); see through it.
  if (const auto *AT = QT->getAs<AtomicType>())
    return hasOrigins(AT->getValueType());
  if (QT->isPointerOrReferenceType() || isGslPointerType(QT))
    return true;
  // An array of pointer-like elements is tracked with a single origin shared
  // across all elements (indices are not disambiguated). A borrow stored into
  // any element is merged into that origin.
  if (QT->isArrayType())
    return hasOrigins(QT->getAsArrayTypeUnsafe()->getElementType());
  if (LifetimeAnnotatedOriginTypes.contains(QT.getCanonicalType().getTypePtr()))
    return true;
  const auto *RD = QT->getAsCXXRecordDecl();
  if (!RD)
    return false;
  // Standard library callable wrappers (e.g., std::function) can propagate the
  // stored lambda's origins.
  if (isStdCallableWrapperType(RD))
    return true;
  // A lambda has origins when any capture has a tracked type; the lambda
  // itself is tracked as a single origin.
  if (RD->isLambda()) {
    for (const auto *FD : RD->fields())
      if (hasOrigins(FD->getType()))
        return true;
    return false;
  }
  // TODO: Unions are not tracked.
  if (RD->isUnion())
    return false;
  for (const auto *FD : RD->fields())
    if (FD->getAccess() == AS_public && hasOrigins(FD->getType()))
      return true;
  return false;
}

bool OriginManager::isTrackedField(const CXXRecordDecl *RD,
                                   const FieldDecl *FD) const {
  return FD->getAccess() == AS_public && hasOrigins(FD->getType()) &&
         isAccessedField(FD);
}

/// Determines if an expression has origins that need to be tracked.
///
/// An expression has origins if:
/// - It's a glvalue (has addressable storage), OR
/// - Its type is pointer-like (pointer, reference, or gsl::Pointer), OR
/// - Its type is registered for origin tracking (e.g., return type of a
/// [[clang::lifetimebound]] function)
///
/// Examples:
/// - `int x; x` : has origin (glvalue)
/// - `int* p; p` : has 2 origins (1 for glvalue and 1 for pointer type)
/// - `std::string_view{}` : has 1 origin (prvalue of pointer type)
/// - `42` : no origin (prvalue of non-pointer type)
/// - `x + y` : (where x, y are int) → no origin (prvalue of non-pointer type)
bool OriginManager::hasOrigins(const Expr *E) const {
  return E->isGLValue() || hasOrigins(E->getType());
}

/// Returns true if the declaration has its own storage that can be borrowed.
///
/// References generally have no storage - they are aliases to other storage.
/// For example:
///   int x;      // has storage (can issue loans to x's storage)
///   int& r = x; // no storage (r is an alias to x's storage)
///   int* p;     // has storage (the pointer variable p itself has storage)
///
/// TODO: Handle lifetime extension. References initialized by temporaries
/// can have storage when the temporary's lifetime is extended:
///   const int& r = 42; // temporary has storage, lifetime extended
///   Foo&& f = Foo{};   // temporary has storage, lifetime extended
/// Currently, this function returns false for all reference types.
bool doesDeclHaveStorage(const ValueDecl *D) {
  // A structured binding aliases an element of its holding object; it has no
  // borrowable storage of its own. The holding object provides the storage
  // (its own, for a by-value copy; the referent's, for a by-reference binding).
  if (isa<BindingDecl>(D))
    return false;
  return !D->getType()->isReferenceType();
}

OriginManager::OriginManager(const AnalysisDeclContext &AC)
    : AST(AC.getASTContext()) {
  runPreScan(AC);
  initializeThisOrigins(AC.getDecl());
}

void OriginManager::initializeThisOrigins(const Decl *D) {
  const auto *MD = llvm::dyn_cast_or_null<CXXMethodDecl>(D);
  if (!MD || !MD->isInstance())
    return;
  // Lambdas can capture 'this' from the surrounding context, but in that case
  // 'this' does not refer to the lambda object itself.
  if (const CXXRecordDecl *P = MD->getParent(); P && P->isLambda())
    return;
  ThisOrigins = buildNodeForType(MD->getThisType(), MD);
}

OriginNode *OriginManager::createNode(const ValueDecl *D, QualType QT) {
  OriginID NewID = getNextOriginID();
  AllOrigins.emplace_back(NewID, D, QT.getTypePtrOrNull());
  return new (Allocator.Allocate<OriginNode>()) OriginNode(NewID);
}

OriginNode *OriginManager::createNode(const Expr *E, QualType QT) {
  OriginID NewID = getNextOriginID();
  AllOrigins.emplace_back(NewID, E, QT.getTypePtrOrNull());
  return new (Allocator.Allocate<OriginNode>()) OriginNode(NewID);
}

OriginNode *OriginManager::createSingleOriginNode(OriginID OID) {
  return new (Allocator.Allocate<OriginNode>()) OriginNode(OID);
}

void OriginManager::attachPointeeChild(OriginNode *Parent,
                                       OriginNode *Pointee) {
  assert(Pointee && "pointee subtree must be non-null");
  auto *E = new (Allocator.Allocate<OriginNode::Edge>())
      OriginNode::Edge{nullptr, Pointee};
  Parent->setChildren({E, 1});
}

void OriginManager::attachChildren(OriginNode *Parent,
                                   llvm::ArrayRef<OriginNode::Edge> Children) {
  Parent->setChildren(Children.copy(Allocator));
}

template <typename T>
OriginNode *OriginManager::buildNodeForType(QualType QT, const T *Node) {
  llvm::SmallPtrSet<const Type *, 4> Visited;
  return buildNodeForTypeImpl(QT, Node, Visited, 0);
}

unsigned OriginManager::getIndirectionDepth(QualType QT) {
  if (!hasOrigins(QT))
    return 0;
  // Measure with the same builder used for declarations; the throwaway tree's
  // pointee-chain length is the indirection depth.
  return buildNodeForType(QT, static_cast<const Expr *>(nullptr))->getLength();
}

template <typename T>
OriginNode *
OriginManager::buildNodeForTypeImpl(QualType QT, const T *Node,
                                    llvm::SmallPtrSet<const Type *, 4> &Visited,
                                    unsigned FieldDepth) {
  assert(hasOrigins(QT) && "buildNodeForType called for type without origins");

  // `_Atomic(T)` is transparent for lifetime purposes: build the node for T.
  if (const auto *AT = QT->getAs<AtomicType>())
    return buildNodeForTypeImpl(AT->getValueType(), Node, Visited, FieldDepth);

  // An array shares a single origin across all of its elements; model it as if
  // it were one element. `arr[i]` accesses (built in getOrCreateNode) reuse
  // this same origin, so a borrow stored into any element is visible through
  // every element.
  if (QT->isArrayType())
    return buildNodeForTypeImpl(QT->getAsArrayTypeUnsafe()->getElementType(),
                                Node, Visited, FieldDepth);

  const auto *RD = QT->getAsCXXRecordDecl();
  const Type *Canonical = QT.getCanonicalType().getTypePtr();
  // Cycle cut: only records enter Visited; re-entering one returns a
  // leaf to stop descending further. Loans landing on the cut leaf are
  // dropped (e.g., through `n->next->next`).
  //
  // Pointer/reference types stay transparent: including them in Visited
  // would make the same record's shape depend on the entry path. E.g.,
  // Node's Sub_next would have length 2 from a Node start but length 1
  // from a Node* start, breaking flow's length assertion.
  if (RD && !Visited.insert(Canonical).second)
    return createNode(Node, QT);

  OriginNode *Head = createNode(Node, QT);

  if (QT->isPointerOrReferenceType()) {
    QualType PointeeTy = QT->getPointeeType();
    // We recurse if the pointee type is pointer-like, to build the next
    // level in the origin tree. E.g., for T*& / View&.
    if (hasOrigins(PointeeTy))
      attachPointeeChild(
          Head, buildNodeForTypeImpl(PointeeTy, Node, Visited, FieldDepth));
  } else if (RD) {
    bool WithinFieldDepthLimit = !MaxFieldDepth || FieldDepth < *MaxFieldDepth;
    bool shouldExpandFields =
        !(isGslPointerType(QT) || isStdCallableWrapperType(RD) ||
          LifetimeAnnotatedOriginTypes.contains(Canonical) || RD->isLambda()) &&
        WithinFieldDepthLimit;
    if (shouldExpandFields) {
      SmallVector<OriginNode::Edge, 4> FieldChildren;
      for (const FieldDecl *F : RD->fields())
        if (isTrackedField(RD, F)) {
          OriginNode *Sub =
              buildNodeForTypeImpl(F->getType(), Node, Visited, FieldDepth + 1);
          FieldChildren.push_back({F, Sub});
        }
      attachChildren(Head, FieldChildren);
    }
  }

  if (RD)
    Visited.erase(Canonical);
  return Head;
}

OriginNode *OriginManager::getOrCreateNode(const ValueDecl *D) {
  if (!hasOrigins(D->getType()))
    return nullptr;
  auto It = DeclToNode.find(D);
  if (It != DeclToNode.end())
    return It->second;
  return DeclToNode[D] = buildNodeForType(D->getType(), D);
}

bool OriginManager::isStableStorageOrigin(const OriginNode *N) const {
  if (!N)
    return false;
  // A decl-anchored access either is the decl-origin itself (reference-typed
  // decls/fields reuse it) or is an lvalue outer node whose pointee is the
  // decl-origin (storage-having decls/fields: `&v` borrows v's slot). Check
  // both N and its pointee against the declaration and `this` anchors.
  const OriginNode *Pointee = N->getPointeeChild();
  for (const auto &KV : DeclToNode)
    if (KV.second == N || (Pointee && KV.second == Pointee))
      return true;
  if (ThisOrigins && (*ThisOrigins == N || *ThisOrigins == Pointee))
    return true;
  return false;
}

OriginNode *OriginManager::getOrCreateNode(const Expr *E) {
  if (auto *ParenIgnored = E->IgnoreParens(); ParenIgnored != E)
    return getOrCreateNode(ParenIgnored);
  // We do not see CFG stmts for ExprWithCleanups. Simply peel them.
  if (const ExprWithCleanups *EWC = dyn_cast<ExprWithCleanups>(E))
    return getOrCreateNode(EWC->getSubExpr());

  if (!hasOrigins(E))
    return nullptr;

  auto It = ExprToNode.find(E);
  if (It != ExprToNode.end())
    return It->second;

  // A statement expression (`({ ...; e; })`) yields the value of its final
  // expression `e`; forward to it so a borrow `e` produces is tracked. A borrow
  // of a body-local escaping this way is still caught: FactsGenerator marks the
  // statement-expression's value as used at the statement-expression's own
  // program point -- after the body's locals expire -- so the loan is live at
  // the local's expiry (see VisitStmtExpr).
  if (const auto *SE = dyn_cast<StmtExpr>(E))
    if (const CompoundStmt *CS = SE->getSubStmt(); CS && !CS->body_empty())
      if (const auto *Last = dyn_cast<Expr>(CS->body_back()))
        if (OriginNode *N = getOrCreateNode(Last))
          return ExprToNode[E] = N;

  QualType Type = E->getType();
  // Special handling for 'this' expressions to share origins with the method's
  // implicit object parameter.
  if (isa<CXXThisExpr>(E) && ThisOrigins)
    return *ThisOrigins;

  // A dereference of an address-of (`*&E`) denotes the very same object as `E`
  // -- the two operators cancel. Reuse `E`'s origins rather than building a
  // fresh, disconnected node, so an access through the round-trip (e.g.
  // `(*&arr)[i] = ...`) routes to the same storage as a plain `arr[i]` and the
  // borrow is tracked.
  if (const auto *Deref = dyn_cast<UnaryOperator>(E);
      Deref && Deref->getOpcode() == UO_Deref)
    if (const auto *AddrOf = dyn_cast<UnaryOperator>(
            Deref->getSubExpr()->IgnoreParenImpCasts());
        AddrOf && AddrOf->getOpcode() == UO_AddrOf)
      if (OriginNode *N = getOrCreateNode(AddrOf->getSubExpr()))
        return ExprToNode[E] = N;

  // A structured binding is an alias for an element of its holding object.
  // Resolve a binding reference to its holding expression (e.g. `e[i]` /
  // `e.field`); the flows for those expressions are generated in
  // handleStructuredBinding. A borrow of the binding (e.g. `&a`) then borrows
  // the holding object's storage -- the copy for a by-value binding (which
  // expires), or the referent for a by-reference binding.
  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    if (auto *BD = dyn_cast<BindingDecl>(DRE->getDecl()))
      if (const Expr *Binding = BD->getBinding())
        return ExprToNode[E] = getOrCreateNode(Binding);

  // Special handling for expressions referring to a decl to share origins with
  // the underlying decl.
  const ValueDecl *ReferencedDecl = nullptr;
  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    ReferencedDecl = DRE->getDecl();
  else if (auto *ME = dyn_cast<MemberExpr>(E)) {
    if (auto *Field = dyn_cast<FieldDecl>(ME->getMemberDecl());
        Field && isThisExpr(ME->getBase()))
      ReferencedDecl = Field;
    // A STATIC data member is a variable, not a subobject: `r.slot` and
    // `R::slot` denote the very same object, and the base is evaluated and
    // discarded. Only the qualified spelling is a DeclRefExpr, so without this
    // the member spelling builds a fresh node disconnected from the variable --
    // a store through it lands on a throwaway expression origin, and the
    // global-escape fact keyed on the VarDecl is never emitted. Share the
    // variable's origins so both spellings behave alike.
    else if (auto *Var = dyn_cast<VarDecl>(ME->getMemberDecl());
             Var && Var->hasGlobalStorage())
      ReferencedDecl = Var;
  }
  if (ReferencedDecl) {
    OriginNode *Head = nullptr;
    // For non-reference declarations (e.g., `int* p`), the expression is an
    // lvalue (addressable) that can be borrowed, so we create an outer origin
    // for the lvalue itself, with the pointee being the declaration's list.
    // This models taking the address: `&p` borrows the storage of `p`, not what
    // `p` points to.
    if (doesDeclHaveStorage(ReferencedDecl)) {
      Head = createNode(E, QualType{});
      // This ensures origin sharing: multiple expressions to the same
      // declaration share the same underlying origins.
      if (OriginNode *ON = getOrCreateNode(ReferencedDecl))
        attachPointeeChild(Head, ON);
    } else {
      // For reference-typed declarations (e.g., `int& r = p`) which have no
      // storage, the DeclRefExpr directly reuses the declaration's list since
      // references don't add an extra level of indirection at the expression
      // level.
      Head = getOrCreateNode(ReferencedDecl);
    }
    return ExprToNode[E] = Head;
  }

  // An array subscript `arr[i]` accesses an element. All elements share the
  // array's single element-origin (indices are not disambiguated), and the
  // array's own lvalue list already has the `[storage] -> [element-value]`
  // shape an element access needs, so reuse it directly. A borrow stored into
  // one element is then visible through every element access. This applies only
  // to genuine array subscripts; a subscript of a *pointer* (e.g. an `int a[]`
  // parameter, which is really `int *`) is an ordinary indirection. (The
  // equivalent `*(arr+i)` form propagates element loans through the normal
  // decay/arithmetic/deref flow; only its store is routed to the shared
  // element-origin, in handleAssignment.)
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    const Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
    if (Base->getType()->isArrayType())
      if (OriginNode *BaseNode = getOrCreateNode(Base))
        return ExprToNode[E] = BaseNode;
  }

  // For a MemberExpr whose base is not `this` (handled above), look up the
  // field child in the base's per-instance origin tree. This makes loans
  // flowing into one occurrence of `s.v` visible at later occurrences.
  if (auto *ME = dyn_cast<MemberExpr>(E))
    if (auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      OriginNode *Base = getOrCreateNode(ME->getBase()->IgnoreParenImpCasts());
      if (Base)
        if (OriginNode *Sub = Base->getFieldChildInChain(FD)) {
          // For non-reference fields (e.g., `View v;` in a record), the
          // MemberExpr `s.v` is an lvalue (addressable) that can be
          // borrowed, so we create an outer origin for the lvalue itself,
          // with the pointee being the field's shared subtree. `&s.v` borrows
          // the storage of the v-slot in s, not what v refers to.
          if (doesDeclHaveStorage(FD)) {
            OriginNode *Outer = createNode(E, QualType{});
            attachPointeeChild(Outer, Sub);
            return ExprToNode[E] = Outer;
          }
          // For reference-typed fields (e.g., `int& r;` in a record) which
          // have no storage, the MemberExpr `s.r` directly reuses the
          // field's subtree.
          return ExprToNode[E] = Sub;
        }
      // The field child was not found because the base's record is a leaf in
      // the origin tree (a gsl::Pointer / owner / lambda / lifetime-annotated
      // type is not field-expanded -- see buildNodeForTypeImpl). Build a fresh
      // node for this member access, but record the base origin as its parent:
      // a borrow captured into the whole enclosing object lives on the base's
      // origin -- unreachable from this fresh node -- so consumers (e.g.
      // assumed-invalidation) can walk the parent chain to find it.
      QualType MemberTy = E->getType();
      if (E->isGLValue() && !MemberTy->isReferenceType())
        MemberTy = AST.getLValueReferenceType(MemberTy);
      OriginNode *N = buildNodeForType(MemberTy, E);
      if (Base)
        N->setParent(Base);
      return ExprToNode[E] = N;
    }

  // If E is an lvalue , it refers to storage. We model this storage as the
  // first level of origin list, as if it were a reference, because l-values are
  // addressable.
  if (E->isGLValue() && !Type->isReferenceType())
    Type = AST.getLValueReferenceType(Type);
  return ExprToNode[E] = buildNodeForType(Type, E);
}

void OriginManager::dump(OriginID OID, llvm::raw_ostream &OS,
                         const FieldDecl *FD) const {
  OS << OID << " (";
  Origin O = getOrigin(OID);
  if (const ValueDecl *VD = O.getDecl()) {
    OS << "Decl: " << VD->getNameAsString();
  } else if (const Expr *E = O.getExpr()) {
    OS << "Expr: " << E->getStmtClassName();
    if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (const ValueDecl *VD = DRE->getDecl())
        OS << ", Decl: " << VD->getNameAsString();
    }
  } else {
    OS << "Unknown";
  }
  if (O.Ty)
    OS << ", Type : " << QualType(O.Ty, 0).getAsString();
  if (FD)
    OS << ", Field: " << FD->getName();
  OS << ")";
}

const Origin &OriginManager::getOrigin(OriginID ID) const {
  assert(ID.Value < AllOrigins.size());
  return AllOrigins[ID.Value];
}

void OriginManager::collectMissingOrigins(Stmt &FunctionBody,
                                          LifetimeSafetyStats &LSStats) {
  MissingOriginCollector Collector(this->ExprToNode, *this, LSStats);
  Collector.TraverseStmt(const_cast<Stmt *>(&FunctionBody));
}

void OriginManager::runPreScan(const AnalysisDeclContext &AC) {
  PreScanCollector Collector;
  if (Stmt *Body = AC.getBody())
    Collector.TraverseStmt(Body);
  if (const auto *CD = dyn_cast<CXXConstructorDecl>(AC.getDecl()))
    for (const auto *Init : CD->inits()) {
      if (const FieldDecl *FD = Init->getAnyMember())
        AccessedFields.insert(FD);
      if (Expr *InitE = Init->getInit())
        Collector.TraverseStmt(InitE);
    }
  for (QualType QT : Collector.getCollectedTypes())
    registerLifetimeAnnotatedOriginType(QT);
  AccessedFields.insert_range(Collector.getAccessedFields());
}

void OriginManager::registerLifetimeAnnotatedOriginType(QualType QT) {
  if (!QT->getAsCXXRecordDecl() || hasOrigins(QT))
    return;

  LifetimeAnnotatedOriginTypes.insert(QT.getCanonicalType().getTypePtr());
}

const OriginNode *
OriginManager::getOriginForAccessPath(const AccessPath &AP) const {
  const OriginNode *N = nullptr;
  if (const auto *VD = dyn_cast_or_null<ValueDecl>(AP.getAsValueDecl())) {
    auto It = DeclToNode.find(VD);
    if (It != DeclToNode.end())
      N = It->second;
  } else if (const ParmVarDecl *PVD = AP.getAsPlaceholderParam()) {
    auto It = DeclToNode.find(PVD);
    if (It != DeclToNode.end())
      N = It->second;
  } else if (AP.getAsPlaceholderThis()) {
    if (auto ThisOrigins = getThisOrigins())
      N = *ThisOrigins;
  }
  if (!N)
    return nullptr;
  for (const PathElement &PE : AP.getElements()) {
    // Descend only as far as the tree distinguishes. A field edge exists when
    // the subobject has an origin of its own, and that is the origin a read of
    // the same member consults, so descending keeps the store and the read on
    // the same node. Where no edge exists -- a `gsl::Pointer` record is one
    // origin for the whole object, and an `Interior` step stands for an unknown
    // subobject -- the enclosing node IS what a read consults, so stopping
    // there lands the store where it will be seen. Never skip past a step onto
    // an unrelated node.
    if (!PE.isField())
      break;
    const OriginNode *Child = N->getFieldChildInChain(PE.getFieldDecl());
    if (!Child)
      break;
    N = Child;
  }
  return N;
}

} // namespace clang::lifetimes::internal

namespace clang::lifetimes {
unsigned getIndirectionDepth(QualType QT, ASTContext &Ctx) {
  // Reuse the analysis's origin-tree builder so this stays in sync with how
  // declarations are modeled; a standalone manager has no body context, so its
  // pointee-chain length is the purely structural indirection depth.
  return internal::OriginManager(Ctx).getIndirectionDepth(QT);
}
} // namespace clang::lifetimes
