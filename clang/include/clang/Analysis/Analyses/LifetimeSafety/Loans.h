//===- Loans.h - Loan and Access Path Definitions --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the Loan and AccessPath structures, which represent
// borrows of storage locations, and the LoanManager, which manages the
// creation and retrieval of loans during lifetime analysis.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_LOANS_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_LOANS_H

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Utils.h"
#include "llvm/Support/raw_ostream.h"

namespace clang::lifetimes::internal {

using LoanID = utils::ID<struct LoanTag>;
inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, LoanID ID) {
  return OS << ID.Value;
}

/// One step of an access path below its root: a named field, or an unnamed
/// interior region.
///
/// `Interior` (printed `.*`) means "some subobject, but the analysis does not
/// know which". It is what a `[[clang::lifetimebound]]` result honestly denotes:
/// the annotation promises the borrow points somewhere inside the object, not
/// that it points at the object itself, and not at any particular field. Naming
/// a concrete field there would assert storage that need not exist (projecting
/// `.a` onto a loan of `o` yields `o.a` even when `a` is a field of `o.in`), and
/// two such fictions can look provably disjoint from the truth.
///
/// Because it stands for an unknown element, `Interior` compares as *may-match*
/// against everything: paths containing it are never provably disjoint. See
/// AccessPath::isPrefixOf / divergesFrom.
///
/// Ported from upstream's field-sensitive AccessPath.
class PathElement {
public:
  enum class Kind { Field, Interior };

  static PathElement getField(const FieldDecl &FD) {
    return PathElement(Kind::Field, &FD);
  }
  static PathElement getInterior() {
    return PathElement(Kind::Interior, nullptr);
  }

  bool isField() const { return K == Kind::Field; }
  bool isInterior() const { return K == Kind::Interior; }
  const FieldDecl *getFieldDecl() const { return FD; }

  /// Exact identity, used for hashing and memoization. To ask whether two
  /// *paths* can denote the same storage, use AccessPath::isPrefixOf, which
  /// expands `Interior` as a wildcard.
  bool operator==(const PathElement &Other) const {
    return K == Other.K && FD == Other.FD;
  }
  bool operator!=(const PathElement &Other) const { return !(*this == Other); }

  void dump(llvm::raw_ostream &OS) const {
    if (isField())
      OS << "." << FD->getNameAsString();
    else
      OS << ".*";
  }

private:
  PathElement(Kind K, const FieldDecl *FD) : K(K), FD(FD) {}
  Kind K;
  const FieldDecl *FD;
};

/// Represents the storage location being borrowed, e.g., a specific stack
/// variable or a field within it: var.field.*
///
/// An AccessPath consists of a root which is one of:
///   - ValueDecl: a local variable or global
///   - MaterializeTemporaryExpr: a temporary object
///   - ParmVarDecl: a function parameter (placeholder)
///   - CXXMethodDecl: the implicit 'this' object (placeholder)
///   - CXXNewExpr: a heap allocation made by `new`
///   - Immortal: storage that lives for the whole program (the return value of
///     a `[[clang::lifetime_immortal]]` function)
///
/// ...followed by a sequence of `PathElement`s naming the subobject borrowed
/// below that root, so `x`, `x.field` and `x.field.*` are distinguishable.
///
/// Placeholder and Immortal paths never expire within the function scope, as
/// they represent storage from the caller's scope or storage that outlives the
/// program's execution.
///
/// TODO: Model access paths of other types, e.g. array subscript, heap
/// allocation not through `new`, and globals.
class AccessPath {
public:
  enum class Kind : uint8_t {
    ValueDecl,
    MaterializeTemporary,
    PlaceholderParam,
    PlaceholderThis,
    NewAllocation,
    Immortal,
    /// The initial, uninitialized contents of an array of pointer-like
    /// elements. Seeded into the array's shared element-origin at declaration
    /// so the origin is never empty (a borrow stored into an element merges in
    /// alongside it). Like the placeholders, it never expires.
    Uninitialized,
    /// An untracked borrow: the result of a borrow-returning operation the
    /// analysis cannot model (e.g. a call returning a view/pointer that is not
    /// [[clang::lifetimebound]] and not a recognized accessor, such as
    /// std::string_view::substr). Carried so that "a borrow was lost here"
    /// survives dataflow joins -- unlike an empty loan set, which a co-resident
    /// valid borrow on another path would mask. Rooted at the producing
    /// expression. Never matches real storage, so it is inert in expiry /
    /// invalidation; the lost-loan check reports it.
    Unknown,
  };

private:
  Kind K;
  llvm::PointerUnion<const Expr *, const Decl *> Root;
  /// The subobject below `Root` that is borrowed. Empty for the root itself.
  llvm::SmallVector<PathElement, 1> Elements;

public:
  AccessPath(const clang::ValueDecl *D) : K(Kind::ValueDecl), Root(D) {}
  AccessPath(const clang::MaterializeTemporaryExpr *MTE)
      : K(Kind::MaterializeTemporary), Root(MTE) {}
  AccessPath(const CXXNewExpr *New) : K(Kind::NewAllocation), Root(New) {}
  /// Creates an extended path by appending one element: `AccessPath(x, .f)` is
  /// the path to `x.f`.
  AccessPath(const AccessPath &Other, PathElement E)
      : K(Other.K), Root(Other.Root), Elements(Other.Elements) {
    Elements.push_back(E);
  }
  static AccessPath Placeholder(const ParmVarDecl *PVD) {
    return AccessPath(Kind::PlaceholderParam, PVD);
  }
  static AccessPath Placeholder(const CXXMethodDecl *MD) {
    return AccessPath(Kind::PlaceholderThis, MD);
  }
  /// Storage that lives for the whole program, identified by the
  /// `[[clang::lifetime_immortal]]` function whose return value created it.
  static AccessPath Immortal(const FunctionDecl *FD) {
    return AccessPath(Kind::Immortal, FD);
  }
  /// A heap allocation produced by an allocating function (e.g. one with
  /// `__attribute__((malloc))`), identified by the call expression.
  static AccessPath HeapAllocation(const clang::Expr *AllocExpr) {
    return AccessPath(Kind::NewAllocation, AllocExpr);
  }
  /// The uninitialized initial contents of an array of pointer-like elements,
  /// identified by the array declaration. Never expires.
  static AccessPath Uninitialized(const clang::ValueDecl *D) {
    return AccessPath(Kind::Uninitialized, D);
  }
  /// An untracked borrow produced by `Producer` (a borrow-returning operation
  /// the analysis cannot model). Reported by the lost-loan check; inert
  /// elsewhere.
  static AccessPath Unknown(const clang::Expr *Producer) {
    return AccessPath(Kind::Unknown, Producer);
  }
  bool isUnknown() const { return K == Kind::Unknown; }
  AccessPath(const AccessPath &Other)
      : K(Other.K), Root(Other.Root), Elements(Other.Elements) {}
  AccessPath &operator=(const AccessPath &) = delete;

  llvm::ArrayRef<PathElement> getElements() const { return Elements; }

  /// True if this path *may* be a prefix of `Other` (or denote the same
  /// storage). `x` is a prefix of `x`, `x.f` and `x.f.g`; `x.f` is not a prefix
  /// of `x.g`.
  ///
  /// An `Interior` (`.*`) element stands for an unknown subobject: ZERO, one or
  /// many member accesses. So `x.*` may denote `x` itself, `x.f`, or `x.f.g`,
  /// and matching is a wildcard match rather than element-by-element equality.
  /// Getting that wrong -- treating `.*` as exactly one element -- makes `x` and
  /// `x.*` different storage, so anything keyed on the pair (moved loans,
  /// diagnostic anchors) stops recognizing them as the same borrow.
  ///
  /// This is a may-analysis: callers use it to decide whether an invalidation
  /// reaches a borrow, where "might" must behave like "does".
  bool isPrefixOf(const AccessPath &Other) const {
    if (K != Other.K || Root != Other.Root)
      return false;
    return elementsMayPrefix(Elements, Other.Elements);
  }

  /// True if the two paths *provably* denote disjoint storage: same root, but
  /// neither may be a prefix of the other (`x.a` vs `x.b`).
  ///
  /// A path containing `.*` is never disjoint from one it might overlap, since
  /// the wildcard may expand to match. Only concrete, definitely-different
  /// elements make two paths disjoint.
  bool divergesFrom(const AccessPath &Other) const {
    return K == Other.K && Root == Other.Root && !isPrefixOf(Other) &&
           !Other.isPrefixOf(*this);
  }

  Kind getKind() const { return K; }

  const clang::ValueDecl *getAsValueDecl() const {
    return K == Kind::ValueDecl
               ? cast<const clang::ValueDecl>(cast<const clang::Decl *>(Root))
               : nullptr;
  }
  const clang::MaterializeTemporaryExpr *getAsMaterializeTemporaryExpr() const {
    return K == Kind::MaterializeTemporary
               ? cast<const MaterializeTemporaryExpr>(
                     cast<const clang::Expr *>(Root))
               : nullptr;
  }
  const ParmVarDecl *getAsPlaceholderParam() const {
    return K == Kind::PlaceholderParam
               ? cast<const ParmVarDecl>(cast<const clang::Decl *>(Root))
               : nullptr;
  }
  const clang::ValueDecl *getAsUninitialized() const {
    return K == Kind::Uninitialized
               ? cast<const clang::ValueDecl>(cast<const clang::Decl *>(Root))
               : nullptr;
  }
  const CXXMethodDecl *getAsPlaceholderThis() const {
    return K == Kind::PlaceholderThis
               ? cast<const CXXMethodDecl>(cast<const clang::Decl *>(Root))
               : nullptr;
  }
  const CXXNewExpr *getAsNewAllocation() const {
    return K == Kind::NewAllocation
               ? dyn_cast_if_present<CXXNewExpr>(
                     Root.dyn_cast<const clang::Expr *>())
               : nullptr;
  }
  /// A heap allocation is `new`/`new[]` or a call to an allocating function
  /// (e.g. one with `__attribute__((malloc))`); the root is the allocating
  /// expression.
  bool isHeapAllocation() const { return K == Kind::NewAllocation; }
  const clang::Expr *getAsHeapAllocation() const {
    return K == Kind::NewAllocation ? Root.dyn_cast<const clang::Expr *>()
                                    : nullptr;
  }
  const FunctionDecl *getAsImmortal() const {
    return K == Kind::Immortal
               ? cast<const FunctionDecl>(cast<const clang::Decl *>(Root))
               : nullptr;
  }

  bool operator==(const AccessPath &RHS) const {
    return K == RHS.K && Root == RHS.Root && Elements == RHS.Elements;
  }
  bool operator!=(const AccessPath &RHS) const { return !(*this == RHS); }
  void dump(llvm::raw_ostream &OS) const;

private:
  /// Wildcard prefix match: can `Pat` describe a prefix of `Path`, treating each
  /// `Interior` element in either sequence as zero or more elements?
  ///
  /// The classic greedy glob algorithm, in O(n): walk both sequences, and on a
  /// wildcard remember where to backtrack to if the rest fails to line up.
  static bool elementsMayPrefix(llvm::ArrayRef<PathElement> Pat,
                                llvm::ArrayRef<PathElement> Path) {
    size_t P = 0, S = 0;
    size_t StarP = static_cast<size_t>(-1), StarS = 0;
    while (S < Path.size()) {
      if (P < Pat.size() && Pat[P].isInterior()) {
        // Wildcard: try matching zero elements first, and record the position so
        // a later mismatch can consume one more.
        StarP = P++;
        StarS = S;
      } else if (P < Pat.size() &&
                 (Path[S].isInterior() || Pat[P] == Path[S])) {
        ++P;
        ++S;
      } else if (StarP != static_cast<size_t>(-1)) {
        // Mismatch after a wildcard: let it absorb one more element.
        P = StarP + 1;
        S = ++StarS;
      } else {
        // `Pat` ran out with elements left in `Path`: it is a proper prefix,
        // which is a match. Anything else is a genuine mismatch.
        return P >= Pat.size();
      }
    }
    // Trailing wildcards may match the empty remainder.
    while (P < Pat.size() && Pat[P].isInterior())
      ++P;
    return P >= Pat.size();
  }

  AccessPath(Kind K, const ParmVarDecl *PVD) : K(K), Root(PVD) {}
  AccessPath(Kind K, const CXXMethodDecl *MD) : K(K), Root(MD) {}
  AccessPath(Kind K, const FunctionDecl *FD) : K(K), Root(FD) {}
  AccessPath(Kind K, const clang::ValueDecl *VD) : K(K), Root(VD) {}
  AccessPath(Kind K, const clang::Expr *E) : K(K), Root(E) {}
};

/// Represents lending a storage location.
///
/// A loan tracks the borrowing relationship created by operations like
/// taking a pointer/reference (&x), creating a view (std::string_view sv = s),
/// or receiving a parameter.
///
/// Examples:
///   - `int* p = &x;` creates a loan to `x`
///   - Parameter loans have no IssueExpr (created at function entry)
class Loan {
  const LoanID ID;
  const AccessPath Path;
  /// The expression that creates the loan, e.g., &x. Null for placeholder
  /// loans.
  const Expr *IssuingExpr;

public:
  Loan(LoanID ID, AccessPath Path, const Expr *IssuingExpr)
      : ID(ID), Path(Path), IssuingExpr(IssuingExpr) {}
  LoanID getID() const { return ID; }
  const AccessPath &getAccessPath() const { return Path; }
  const Expr *getIssuingExpr() const { return IssuingExpr; }
  void dump(llvm::raw_ostream &OS) const;
};

/// Manages the creation, storage and retrieval of loans.
class LoanManager {
  using ProjectionCacheKey = std::pair<LoanID, PathElement>;

public:
  LoanManager() = default;

  Loan *createLoan(AccessPath Path, const Expr *IssueExpr) {
    void *Mem = LoanAllocator.Allocate<Loan>();
    auto *NewLoan = new (Mem) Loan(getNextLoanID(), Path, IssueExpr);
    AllLoans.push_back(NewLoan);
    return NewLoan;
  }

  /// Gets or creates the loan obtained by projecting `BaseLoanID` through
  /// `Element`, i.e. the loan to `<base path>.<element>`, issued by
  /// `ProjectingExpr`. Memoized: projecting the same loan with the same element
  /// must yield the same LoanID, or the loan-propagation dataflow would never
  /// reach a fixpoint.
  Loan *getOrCreateProjectedLoan(LoanID BaseLoanID, PathElement Element,
                                 const Expr *ProjectingExpr);

  /// The loan `ProjectedLoanID` was projected from, if it was projected at all.
  std::optional<LoanID> getBaseLoan(LoanID ProjectedLoanID) const;

  const Loan *getLoan(LoanID ID) const {
    assert(ID.Value < AllLoans.size());
    return AllLoans[ID.Value];
  }

  llvm::ArrayRef<const Loan *> getLoans() const { return AllLoans; }

private:
  LoanID getNextLoanID() { return NextLoanID++; }

  /// Memo for getOrCreateProjectedLoan; see there for why it is required.
  llvm::DenseMap<ProjectionCacheKey, Loan *> LoanProjectionCache;
  /// Maps a projected loan back to the loan it was projected from.
  llvm::DenseMap<LoanID, LoanID> BaseLoansMap;

  LoanID NextLoanID{0};
  /// TODO(opt): Profile and evaluate the usefullness of small buffer
  /// optimisation.
  llvm::SmallVector<const Loan *> AllLoans;
  llvm::BumpPtrAllocator LoanAllocator;
};
} // namespace clang::lifetimes::internal

namespace llvm {
template <> struct DenseMapInfo<clang::lifetimes::internal::PathElement> {
  using PathElement = clang::lifetimes::internal::PathElement;
  static PathElement getEmptyKey() {
    return PathElement::getField(
        *DenseMapInfo<const clang::FieldDecl *>::getEmptyKey());
  }
  static PathElement getTombstoneKey() {
    return PathElement::getField(
        *DenseMapInfo<const clang::FieldDecl *>::getTombstoneKey());
  }
  static unsigned getHashValue(const PathElement &Val) {
    return llvm::hash_combine(Val.isInterior(), Val.getFieldDecl());
  }
  static bool isEqual(const PathElement &LHS, const PathElement &RHS) {
    return LHS == RHS;
  }
};
} // namespace llvm

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_LOANS_H
