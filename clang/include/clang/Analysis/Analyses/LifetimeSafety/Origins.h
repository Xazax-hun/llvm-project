//===- Origins.h - Origin and Origin Management ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines Origins, which represent the set of possible loans a
// pointer-like object could hold, and the OriginManager, which manages the
// creation, storage, and retrieval of origins for variables and expressions.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_ORIGINS_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_ORIGINS_H

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/TypeBase.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeStats.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Utils.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"

namespace clang::lifetimes::internal {

class AccessPath;

using OriginID = utils::ID<struct OriginTag>;

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, OriginID ID) {
  return OS << ID.Value;
}

/// An Origin is a symbolic identifier that represents the set of possible
/// loans a pointer-like object could hold at any given time.
///
/// Each Origin corresponds to a single level of indirection. For complex types
/// with multiple levels of indirection (e.g., `int**`), multiple Origins are
/// organized into a tree structure (see below).
struct Origin {
  OriginID ID;
  /// A pointer to the AST node that this origin represents. This union
  /// distinguishes between origins from declarations (variables or parameters)
  /// and origins from expressions.
  llvm::PointerUnion<const clang::ValueDecl *, const clang::Expr *> Ptr;

  /// The type at this indirection level.
  ///
  /// For `int** pp`:
  ///   Root origin: QT = `int**` (what pp points to)
  ///   Pointee origin: QT = `int*` (what *pp points to)
  ///
  /// Null for synthetic lvalue origins (e.g., outer origin of DeclRefExpr).
  const Type *Ty;

  Origin(OriginID ID, const clang::ValueDecl *D, const Type *QT)
      : ID(ID), Ptr(D), Ty(QT) {}
  Origin(OriginID ID, const clang::Expr *E, const Type *QT)
      : ID(ID), Ptr(E), Ty(QT) {}

  const clang::ValueDecl *getDecl() const {
    return Ptr.dyn_cast<const clang::ValueDecl *>();
  }
  const clang::Expr *getExpr() const {
    return Ptr.dyn_cast<const clang::Expr *>();
  }
};

/// A tree of origins representing the structure of a pointer-like or
/// record type.
///
/// Each node carries an OriginID and is connected to children via labeled
/// edges: either a pointee edge (one level of pointer/reference indirection)
/// or a field edge (a named field of a record). Pointer-like types form a
/// pointee chain; record types fan out via field edges.
///
/// Examples:
///   - For `int& x`, the chain has length 2:
///     * Outer: origin for the reference storage itself (the lvalue `x`)
///     * Inner: origin for what `x` refers to
///
///   - For `int* p`, the chain has length 2:
///     * Outer: origin for the pointer variable `p`
///     * Inner: origin for what `p` points to
///
///   - For `View v` (where View is gsl::Pointer), the chain has length 2:
///     * Outer: origin for the view object itself
///     * Inner: origin for what the view refers to
///
///   - For `int** pp`, the chain has length 3:
///     * Outer: origin for `pp` itself
///     * Inner: origin for `*pp` (what `pp` points to)
///     * Inner->Inner: origin for `**pp` (what `*pp` points to)
///
/// The structure enables the analysis to track how loans flow through
/// levels of indirection and across record fields when assignments and
/// dereferences occur.
class OriginNode {
public:
  /// A labeled edge from this node to a child. The label distinguishes how
  /// the child is reached: a null `FD` means a pointee edge (one level of
  /// pointer/reference indirection); a non-null `FD` means a field edge
  /// (the named field of a record). Putting the label on the edge lets
  /// one child node play different roles per parent. For example, the subtree
  /// for `s`'s `v` field is reached from `s`'s record (FD=v) and from
  /// the lvalue outer built for the MemberExpr `s.v` (FD=null).
  struct Edge {
    const FieldDecl *FD;
    OriginNode *Child;
  };

  OriginNode(OriginID OID) : OID(OID) {}

  OriginID getOriginID() const { return OID; }

  llvm::ArrayRef<Edge> children() const { return Children; }

  OriginNode *getPointeeChild() const {
    for (const Edge &E : Children)
      if (!E.FD)
        return E.Child;
    return nullptr;
  }

  OriginNode *getFieldChild(const FieldDecl *F) const {
    assert(F);
    for (const Edge &E : Children)
      if (E.FD == F)
        return E.Child;
    return nullptr;
  }

  OriginNode *getFieldChildInChain(const FieldDecl *FD) const {
    for (const OriginNode *N = this; N; N = N->getPointeeChild())
      if (OriginNode *Child = N->getFieldChild(FD))
        return Child;
    return nullptr;
  }

  void setChildren(llvm::ArrayRef<Edge> NewChildren) {
    assert(Children.empty() && "children must be set at most once");
    Children = NewChildren;
  }

  /// The enclosing-object origin this node was reached through, when that
  /// object is a leaf in the origin tree (a gsl::Pointer / owner / lambda /
  /// lifetime-annotated record is not field-expanded -- see
  /// buildNodeForTypeImpl). A member access `w.in` of such a leaf base builds a
  /// fresh, disconnected node; its parent records `w`'s origin so a borrow that
  /// flowed into the whole object (which lives on `w`'s origin, unreachable
  /// from this node) can still be found -- e.g. for invalidation -- without
  /// fragile AST inspection of the base expression. Null for nodes that are
  /// genuine descendants of their enclosing object (plain-record field
  /// subtrees) or have no enclosing object.
  OriginNode *getParent() const { return Parent; }
  void setParent(OriginNode *P) { Parent = P; }

  // Used for assertion checks only (to ensure pointee chains have matching
  // lengths).
  size_t getLength() const {
    size_t Length = 1;
    const OriginNode *T = this;
    while (auto *ON = T->getPointeeChild()) {
      T = ON;
      Length++;
    }
    return Length;
  }

private:
  OriginID OID;
  llvm::ArrayRef<Edge> Children;
  OriginNode *Parent = nullptr;
};

bool doesDeclHaveStorage(const ValueDecl *D);

/// Manages the creation, storage, and retrieval of origins for pointer-like
/// variables and expressions.
class OriginManager {
public:
  explicit OriginManager(const AnalysisDeclContext &AC);

  /// Lightweight constructor for standalone, type-structural queries (namely
  /// getIndirectionDepth) outside a function analysis: it sets up only what the
  /// origin-tree builder needs and skips the body pre-scan and `this`-origin
  /// setup. Do not use it to analyze a function.
  explicit OriginManager(ASTContext &Ctx) : AST(Ctx) {}

  /// Gets or creates the OriginNode for a given ValueDecl.
  ///
  /// Creates a tree structure mirroring the levels of indirection in the
  /// declaration's type (e.g., `int* p` creates a chain of length 2).
  ///
  /// \returns The OriginNode, or nullptr if the type is not pointer-like.
  OriginNode *getOrCreateNode(const ValueDecl *D);

  /// Gets or creates the OriginNode for a given Expr.
  ///
  /// Creates a tree structure based on the expression's type and value
  /// category:
  /// - Lvalues get an implicit reference level (modeling addressability)
  /// - Rvalues of non-pointer type return nullptr (no trackable origin)
  /// - DeclRefExpr may reuse the underlying declaration's tree
  ///
  /// \returns The OriginNode, or nullptr for non-pointer rvalues.
  OriginNode *getOrCreateNode(const Expr *E);

  /// Wraps an existing OriginID in a new single-element OriginNode, so a fact
  /// can refer to a single level of an existing OriginNode.
  OriginNode *createSingleOriginNode(OriginID OID);

  /// Creates a fresh, detached single-level origin (not tied to any declaration
  /// or expression), for synthesizing a loan that has no AST anchor -- e.g. a
  /// borrow into a variable captured by reference into a lambda, which has no
  /// DeclRefExpr at the call site. The origin has a null type, like a synthetic
  /// lvalue origin.
  OriginNode *createDetachedOrigin() {
    OriginID NewID = getNextOriginID();
    AllOrigins.emplace_back(NewID, static_cast<const Expr *>(nullptr), nullptr);
    return createSingleOriginNode(NewID);
  }

  /// Returns the OriginNode for the implicit 'this' parameter if the current
  /// declaration is an instance method.
  std::optional<OriginNode *> getThisOrigins() const { return ThisOrigins; }

  /// Whether \p N is an origin that later reads of the same storage re-resolve
  /// to -- i.e. it is anchored to a declaration (a local/param/field, tracked
  /// in DeclToNode) or to `this`, directly or as the pointee of the lvalue
  /// outer node built for such an access. A borrow merged into such an origin
  /// is observable at a later read; a borrow merged into a transient
  /// expression origin (a conditional `(c?a:b)`, comma, call result, or
  /// temporary) is not, because reads of the underlying objects route to their
  /// own origins, never to that throwaway node.
  bool isStableStorageOrigin(const OriginNode *N) const;

  const Origin &getOrigin(OriginID ID) const;

  llvm::ArrayRef<Origin> getOrigins() const { return AllOrigins; }

  unsigned getNumOrigins() const { return NextOriginID.Value; }

  bool hasOrigins(QualType QT) const;
  bool hasOrigins(const Expr *E) const;

  /// The pointee-chain length of \p QT's origin tree -- the depth metric used
  /// by the single-indirection rule. Returns 0 if the type bears no origins, 1
  /// for a single indirection (e.g. int*, std::string_view), and >1 for a
  /// multi-level indirection (e.g. int**, std::string_view& / std::string_view*
  /// -- a reference/pointer to a view). Builds a throwaway origin tree so a
  /// type with no declaration (such as a function return type) is measured
  /// exactly like a parameter or local of the same type.
  unsigned getIndirectionDepth(QualType QT);

  bool isAccessedField(const FieldDecl *FD) const {
    return AccessedFields.contains(FD);
  }

  void dump(OriginID OID, llvm::raw_ostream &OS,
            const FieldDecl *FD = nullptr) const;

  /// Collects statistics about expressions that lack associated origins.
  void collectMissingOrigins(Stmt &FunctionBody, LifetimeSafetyStats &LSStats);

  /// Returns the origin lists created for declarations (parameters and locals).
  /// Used by soundness checks (e.g. detecting multi-level indirection).
  const llvm::DenseMap<const clang::ValueDecl *, OriginNode *> &
  getDeclOriginLists() const {
    return DeclToNode;
  }

  /// The origin holding what \p AP names, or null if the path does not resolve
  /// to one.
  ///
  /// A loan names storage as a root plus a path of subobject steps. The root is
  /// a declaration, or -- for caller-provided storage -- a placeholder standing
  /// for a parameter or the implicit object; each has an origin. Field steps
  /// then follow the origin tree's field edges, so `h.v` resolves to the very
  /// origin a later read of `h.v` consults. That is what makes a store
  /// *through* a member lvalue visible afterwards, rather than landing in the
  /// throwaway origin of the expression that designated it.
  ///
  /// Descent stops where the origin tree stops distinguishing subobjects: a
  /// `gsl::Pointer` record is a single origin for the whole object, and an
  /// `Interior` step stands for an unknown subobject. The enclosing origin is
  /// then what a read consults too, so it is the right -- and conservative --
  /// answer. Null means the ROOT did not resolve, which callers must treat as a
  /// store they cannot track.
  const OriginNode *getOriginForAccessPath(const AccessPath &AP) const;

private:
  OriginID getNextOriginID() { return NextOriginID++; }

  OriginNode *createNode(const ValueDecl *D, QualType QT);
  OriginNode *createNode(const Expr *E, QualType QT);

  void attachPointeeChild(OriginNode *Parent, OriginNode *Pointee);
  void attachChildren(OriginNode *Parent,
                      llvm::ArrayRef<OriginNode::Edge> Children);

  template <typename T>
  OriginNode *buildNodeForType(QualType QT, const T *Node);
  template <typename T>
  OriginNode *buildNodeForTypeImpl(QualType QT, const T *Node,
                                   llvm::SmallPtrSet<const Type *, 4> &Visited,
                                   unsigned FieldDepth);

  /// Whether a record field participates in origin tracking. Plain records
  /// only track public fields; lambdas track all fields.
  bool isTrackedField(const CXXRecordDecl *RD, const FieldDecl *FD) const;

  void initializeThisOrigins(const Decl *D);

  /// Pre-scans the function body (and constructor init lists) to discover:
  ///
  /// 1. Return types of lifetime-annotated calls (currently
  ///    [[clang::lifetimebound]]), registering them for origin tracking.
  ///
  /// 2. The fields it accesses; the rest are excluded from origin tracking.
  void runPreScan(const AnalysisDeclContext &AC);
  void registerLifetimeAnnotatedOriginType(QualType QT);

  ASTContext &AST;
  OriginID NextOriginID{0};
  /// TODO(opt): Profile and evaluate the usefulness of small buffer
  /// optimisation.
  llvm::SmallVector<Origin> AllOrigins;
  llvm::BumpPtrAllocator Allocator;
  llvm::DenseMap<const clang::ValueDecl *, OriginNode *> DeclToNode;
  llvm::DenseMap<const clang::Expr *, OriginNode *> ExprToNode;
  std::optional<OriginNode *> ThisOrigins;
  /// Types that are not inherently pointer-like but require origin tracking
  /// because of lifetime annotations (currently [[clang::lifetimebound]]) on
  /// functions that return them.
  llvm::DenseSet<const Type *> LifetimeAnnotatedOriginTypes;
  /// Fields accessed in the function body (or constructor init lists).
  /// Fields outside this set are excluded from origin tracking.
  llvm::SmallPtrSet<const FieldDecl *, 8> AccessedFields;

  /// Field-edge depth limit when building origin trees for record types:
  ///   - `std::nullopt`: no limit (full field tree).
  ///   - `0`: disable field tracking (records become single-origin).
  ///   - `N > 0`: track up to N levels of field edges.
  /// Pointee edges are not subject to this limit.
  std::optional<size_t> MaxFieldDepth = std::nullopt;
};
} // namespace clang::lifetimes::internal

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_ORIGINS_H
