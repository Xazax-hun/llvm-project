//===- Facts.h - Lifetime Analysis Facts and Fact Manager ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines Facts, which are atomic lifetime-relevant events (such as
// loan issuance, loan expiration, origin flow, and use), and the FactManager,
// which manages the storage and retrieval of facts for each CFG block.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_FACTS_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_FACTS_H

#include "clang/AST/Decl.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Loans.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Origins.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Utils.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"
#include <cstdint>
#include <optional>

namespace clang::lifetimes::internal {

using FactID = utils::ID<struct FactTag>;

/// An abstract base class for a single, atomic lifetime-relevant event.
class Fact {

public:
  enum class Kind : uint8_t {
    /// A new loan is issued from a borrow expression (e.g., &x).
    Issue,
    /// A loan expires as its underlying storage is freed (e.g., variable goes
    /// out of scope).
    Expire,
    /// An origin is propagated from a source to a destination (e.g., p = q).
    /// This can also optionally kill the destination origin before flowing into
    /// it. Otherwise, the source's loan set is merged into the destination's
    /// loan set.
    OriginFlow,
    /// An origin is used (eg. appears as l-value expression like DeclRefExpr).
    Use,
    /// An origin that is moved (e.g., passed to an rvalue reference parameter).
    MovedOrigin,
    /// A marker for a specific point in the code, for testing.
    TestPoint,
    /// An origin that escapes the function scope (e.g., via return).
    OriginEscapes,
    /// An origin is invalidated (e.g. vector resized, `delete` called).
    InvalidateOrigin,
    /// All loans of an origin are cleared.
    KillOrigin,
    /// A construct the analysis cannot fully model. Drives the "safe
    /// programming model" soundness warnings and carries no dataflow state.
    UntrackedConstruct,
    /// A borrow stored into a view/pointer data member. Used by the checker to
    /// detect self-referential objects (the stored value borrows the same
    /// object that holds the member). Carries no dataflow state.
    FieldStore,
    /// A pair of call arguments where one may be mutated by the call and the
    /// other borrows it. Used by the checker to detect overlapping (aliasing)
    /// arguments. Carries no dataflow state.
    ArgumentOverlap,
  };

private:
  Kind K;
  FactID ID;

protected:
  Fact(Kind K) : K(K) {}

public:
  virtual ~Fact() = default;
  Kind getKind() const { return K; }

  void setID(FactID ID) { this->ID = ID; }
  FactID getID() const { return ID; }

  template <typename T> const T *getAs() const {
    if (T::classof(this))
      return static_cast<const T *>(this);
    return nullptr;
  }

  virtual void dump(llvm::raw_ostream &OS, const LoanManager &,
                    const OriginManager &) const;
};

/// A `ProgramPoint` identifies a location in the CFG by pointing to a specific
/// `Fact`. identified by a lifetime-related event (`Fact`).
///
/// A `ProgramPoint` has "after" semantics: it represents the location
/// immediately after its corresponding `Fact`.
using ProgramPoint = const Fact *;

class IssueFact : public Fact {
  LoanID LID;
  OriginID OID;

public:
  static bool classof(const Fact *F) { return F->getKind() == Kind::Issue; }

  IssueFact(LoanID LID, OriginID OID) : Fact(Kind::Issue), LID(LID), OID(OID) {}
  LoanID getLoanID() const { return LID; }
  OriginID getOriginID() const { return OID; }
  void dump(llvm::raw_ostream &OS, const LoanManager &LM,
            const OriginManager &OM) const override;
};

/// When an AccessPath expires (e.g., a variable goes out of scope), all loans
/// that are associated with this path expire. For example, if `x` expires, then
/// the loan to `x` expires.
class ExpireFact : public Fact {
  // The access path that expires.
  AccessPath AP;

  // Expired origin (e.g., its variable goes out of scope).
  std::optional<OriginID> OID;
  SourceLocation ExpiryLoc;

public:
  static bool classof(const Fact *F) { return F->getKind() == Kind::Expire; }

  ExpireFact(AccessPath AP, SourceLocation ExpiryLoc,
             std::optional<OriginID> OID = std::nullopt)
      : Fact(Kind::Expire), AP(AP), OID(OID), ExpiryLoc(ExpiryLoc) {}

  const AccessPath &getAccessPath() const { return AP; }
  std::optional<OriginID> getOriginID() const { return OID; }
  SourceLocation getExpiryLoc() const { return ExpiryLoc; }

  void dump(llvm::raw_ostream &OS, const LoanManager &LM,
            const OriginManager &OM) const override;
};

class OriginFlowFact : public Fact {
  OriginID OIDDest;
  OriginID OIDSrc;
  // True if the destination origin should be killed (i.e., its current loans
  // cleared) before the source origin's loans are flowed into it.
  bool KillDest;

public:
  static bool classof(const Fact *F) {
    return F->getKind() == Kind::OriginFlow;
  }

  OriginFlowFact(OriginID OIDDest, OriginID OIDSrc, bool KillDest)
      : Fact(Kind::OriginFlow), OIDDest(OIDDest), OIDSrc(OIDSrc),
        KillDest(KillDest) {}

  OriginID getDestOriginID() const { return OIDDest; }
  OriginID getSrcOriginID() const { return OIDSrc; }
  bool getKillDest() const { return KillDest; }

  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

/// Represents that an origin escapes the current scope through various means.
/// This is the base class for different escape scenarios.
class OriginEscapesFact : public Fact {
  OriginID OID;

public:
  /// The way an origin can escape the current scope.
  enum class EscapeKind : uint8_t {
    Return, /// Escapes via return statement.
    Field,  /// Escapes via assignment to a field.
    Global, /// Escapes via assignment to global storage.
    CapturedByThis, /// Captured into the implicit object via
                    /// [[clang::lifetime_capture_by(this)]].
  } EscKind;

  static bool classof(const Fact *F) {
    return F->getKind() == Kind::OriginEscapes;
  }

  OriginEscapesFact(OriginID OID, EscapeKind EscKind)
      : Fact(Kind::OriginEscapes), OID(OID), EscKind(EscKind) {}
  OriginID getEscapedOriginID() const { return OID; }
  EscapeKind getEscapeKind() const { return EscKind; }
};

/// Represents that an origin escapes via a return statement.
class ReturnEscapeFact : public OriginEscapesFact {
  const Expr *ReturnExpr;

public:
  ReturnEscapeFact(OriginID OID, const Expr *ReturnExpr)
      : OriginEscapesFact(OID, EscapeKind::Return), ReturnExpr(ReturnExpr) {}

  static bool classof(const Fact *F) {
    return F->getKind() == Kind::OriginEscapes &&
           static_cast<const OriginEscapesFact *>(F)->getEscapeKind() ==
               EscapeKind::Return;
  }
  const Expr *getReturnExpr() const { return ReturnExpr; };
  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

/// Represents that an origin escapes via assignment to a field.
/// Example: `this->view = local_var;` where local_var outlives the assignment
/// but not the object containing the field.
class FieldEscapeFact : public OriginEscapesFact {
  const FieldDecl *FDecl;

public:
  FieldEscapeFact(OriginID OID, const FieldDecl *FDecl)
      : OriginEscapesFact(OID, EscapeKind::Field), FDecl(FDecl) {}

  static bool classof(const Fact *F) {
    return F->getKind() == Kind::OriginEscapes &&
           static_cast<const OriginEscapesFact *>(F)->getEscapeKind() ==
               EscapeKind::Field;
  }
  const FieldDecl *getFieldDecl() const { return FDecl; };
  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

/// Represents that an origin escapes via assignment to global or static
/// storage. Example: `global_storage = local_var;`
class GlobalEscapeFact : public OriginEscapesFact {
  const VarDecl *Global;

public:
  GlobalEscapeFact(OriginID OID, const VarDecl *VDecl)
      : OriginEscapesFact(OID, EscapeKind::Global), Global(VDecl) {}

  static bool classof(const Fact *F) {
    return F->getKind() == Kind::OriginEscapes &&
           static_cast<const OriginEscapesFact *>(F)->getEscapeKind() ==
               EscapeKind::Global;
  }
  const VarDecl *getGlobal() const { return Global; };
  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

/// Represents an origin captured into the implicit object (`this`) via a
/// [[clang::lifetime_capture_by(this)]] parameter at a call site -- e.g.
/// `obj.capture(arg)` where `capture`'s parameter is annotated
/// capture_by(this). The captured borrow becomes reachable from the (caller's)
/// object, so if it is a [[clang::noescape]] parameter of the analyzed
/// function, that noescape promise is violated. Unlike a Field escape this
/// carries no specific member (the capture target is the whole object), so it
/// records the capturing call expression for the diagnostic.
class CapturedByThisEscapeFact : public OriginEscapesFact {
  const Expr *CaptureExpr;

public:
  CapturedByThisEscapeFact(OriginID OID, const Expr *CaptureExpr)
      : OriginEscapesFact(OID, EscapeKind::CapturedByThis),
        CaptureExpr(CaptureExpr) {}

  static bool classof(const Fact *F) {
    return F->getKind() == Kind::OriginEscapes &&
           static_cast<const OriginEscapesFact *>(F)->getEscapeKind() ==
               EscapeKind::CapturedByThis;
  }
  const Expr *getCaptureExpr() const { return CaptureExpr; };
  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

class UseFact : public Fact {
  const Expr *UseExpr;
  const OriginNode *ONode;
  // True if this use is a write operation (e.g., left-hand side of assignment).
  // Write operations are exempted from use-after-free checks.
  bool IsWritten = false;
  // For an implicit use that has no source expression (e.g. an object's
  // non-trivial destructor reading a borrow it holds, at scope exit): the
  // location to anchor the diagnostic at. Invalid for ordinary uses, which use
  // `UseExpr`'s location instead.
  SourceLocation ImplicitLoc;

public:
  static bool classof(const Fact *F) { return F->getKind() == Kind::Use; }

  UseFact(const Expr *UseExpr, const OriginNode *ONode)
      : Fact(Kind::Use), UseExpr(UseExpr), ONode(ONode) {}
  UseFact(SourceLocation ImplicitLoc, const OriginNode *ONode)
      : Fact(Kind::Use), UseExpr(nullptr), ONode(ONode),
        ImplicitLoc(ImplicitLoc) {}

  const OriginNode *getUsedOrigins() const { return ONode; }
  void setUsedOrigins(const OriginNode *NewONode) { ONode = NewONode; }
  const Expr *getUseExpr() const { return UseExpr; }
  /// True if this is an implicit use with no source expression (a non-trivial
  /// destructor reading a borrow at scope exit); use getImplicitLoc() to anchor
  /// diagnostics.
  bool isImplicit() const { return UseExpr == nullptr; }
  SourceLocation getImplicitLoc() const { return ImplicitLoc; }
  void markAsWritten() { IsWritten = true; }
  bool isWritten() const { return IsWritten; }

  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

/// How strongly the checker must confirm, from the loans the mutated origin
/// actually carries, that it really denotes a mutable owner. Needed when the
/// *static* type of the mutated receiver/argument does not reveal one, so the
/// confirmation must come from what the origin refers to -- robust to
/// references/pointers/ternaries the static type cannot express. Only the loans
/// that pass the gate are treated as invalidated.
enum class OwnerLoanGate : uint8_t {
  /// No confirmation needed: the static type already showed a mutable owner.
  None,
  /// The mutated owner must be REACHABLE from what a loan denotes: either the
  /// loan's record is-a the static type and is (or contains) a mutable owner, or
  /// that record has a by-value subobject derived from the static type which is
  /// (or contains) one -- the case where the loan roots at an *enclosing* object
  /// (`Wrapper w; Base& b = w.d; b.grow();` roots b's loan at `w`). Used for a
  /// receiver whose static type hides the owner.
  ReachableOwner,
  /// Stricter: a loan must DENOTE the mutated object itself -- its record is-a
  /// the static type AND is (or contains) a mutable owner. The enclosing-object
  /// fallback is deliberately NOT accepted here: a member access inherits its
  /// enclosing object's loan and gets no loan of its own (`this->b` carries only
  /// `$this`), so accepting the fallback cannot tell "the argument is this object
  /// viewed as a base" from "the argument is some member of it" -- and only the
  /// former justifies invalidating borrows that carry the enclosing loan.
  /// Used for the dynamic-dispatch parameter case.
  DenotedOwner,
};

/// Represents that an origin's storage has been invalidated by a container
/// operation (e.g., vector::push_back may reallocate, invalidating iterators).
/// Created when a container method that may invalidate references/iterators
/// is called on the container.
class InvalidateOriginFact : public Fact {
  OriginID OID;
  /// The operation that invalidates the origin. Usually an Expr (a mutating
  /// call, `delete`, ...), but may be a non-Expr Stmt -- e.g. the trigger
  /// statement of a destructor, which the intra-procedural analysis treats as
  /// an assumed invalidation when an RAII object captured a mutable owner.
  const Stmt *InvalidationOp;
  /// True when the invalidation is only *assumed* (a non-const operation on an
  /// owner, or passing an owner to a non-const pointer/reference parameter)
  /// rather than a known container-mutation method. Drives the lower-confidence
  /// "assumed invalidation" soundness diagnostic.
  bool Assumed;
  /// True when the invalidation is a deallocation (`delete`, `free`,
  /// `std::destroy_at`, a destructor). Drives the "naked deallocation" check.
  bool Deallocation;
  /// When non-null, the invalidation is scoped to a specific owner field
  /// (e.g. `s.buf.append(...)`): only borrows of *this* field are invalidated,
  /// not the enclosing object or its sibling fields. The receiver origin also
  /// carries the enclosing-object loan (needed for accessor verification),
  /// which must not be treated as invalidated by a field mutation.
  const FieldDecl *MutatedField;
  /// How strongly the checker must confirm, from the loans the mutated origin
  /// actually carries, that it really denotes a mutable owner. Used when the
  /// *static* type of the mutated receiver/argument does not reveal one.
  OwnerLoanGate LoanGate;

public:
  static bool classof(const Fact *F) {
    return F->getKind() == Kind::InvalidateOrigin;
  }

  InvalidateOriginFact(OriginID OID, const Stmt *InvalidationOp,
                       bool Assumed = false, bool Deallocation = false,
                       const FieldDecl *MutatedField = nullptr,
                       OwnerLoanGate LoanGate = OwnerLoanGate::None)
      : Fact(Kind::InvalidateOrigin), OID(OID),
        InvalidationOp(InvalidationOp), Assumed(Assumed),
        Deallocation(Deallocation), MutatedField(MutatedField),
        LoanGate(LoanGate) {}

  OriginID getInvalidatedOrigin() const { return OID; }
  /// The invalidating operation as a statement (never null). Use this for the
  /// diagnostic location/range, which every Stmt provides.
  const Stmt *getInvalidationStmt() const { return InvalidationOp; }
  /// The invalidating operation when it is an expression (a call / `delete`),
  /// or null for a non-Expr operation (a destructor trigger statement).
  const Expr *getInvalidationExpr() const {
    return dyn_cast_or_null<Expr>(InvalidationOp);
  }
  bool isAssumed() const { return Assumed; }
  bool isDeallocation() const { return Deallocation; }
  const FieldDecl *getMutatedField() const { return MutatedField; }
  bool requiresOwnerLoanTarget() const {
    return LoanGate != OwnerLoanGate::None;
  }
  OwnerLoanGate getOwnerLoanGate() const { return LoanGate; }
  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

/// Top-level origin of the expression which was found to be moved, e.g, when
/// being used as an argument to an r-value reference parameter.
class MovedOriginFact : public Fact {
  const OriginID MovedOrigin;
  const Expr *MoveExpr;

public:
  static bool classof(const Fact *F) {
    return F->getKind() == Kind::MovedOrigin;
  }

  MovedOriginFact(const Expr *MoveExpr, OriginID MovedOrigin)
      : Fact(Kind::MovedOrigin), MovedOrigin(MovedOrigin), MoveExpr(MoveExpr) {}

  OriginID getMovedOrigin() const { return MovedOrigin; }
  const Expr *getMoveExpr() const { return MoveExpr; }

  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

/// A dummy-fact used to mark a specific point in the code for testing.
/// It is generated by recognizing a `void("__lifetime_test_point_...")` cast.
class TestPointFact : public Fact {
  StringRef Annotation;

public:
  static bool classof(const Fact *F) { return F->getKind() == Kind::TestPoint; }

  explicit TestPointFact(StringRef Annotation)
      : Fact(Kind::TestPoint), Annotation(Annotation) {}

  StringRef getAnnotation() const { return Annotation; }

  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &) const override;
};

/// All loans are cleared from an origin (e.g., assigning a callable without
/// tracked origins to std::function).
class KillOriginFact : public Fact {
  OriginID OID;

public:
  static bool classof(const Fact *F) {
    return F->getKind() == Kind::KillOrigin;
  }

  KillOriginFact(OriginID OID) : Fact(Kind::KillOrigin), OID(OID) {}

  OriginID getKilledOrigin() const { return OID; }

  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

/// The kind of construct that the analysis could not fully model. Each value
/// maps to a "safe programming model" soundness warning emitted by the
/// checker.
enum class UntrackedConstructReason : uint8_t {
  /// A call whose callee could not be resolved to a function, e.g. a call
  /// through a function or member-function pointer. Such callees cannot carry
  /// lifetime annotations, so the call is not modeled.
  IndirectCall,
  /// An argument bound to a pointer/reference parameter that carries none of
  /// the lifetime annotations ('lifetimebound', 'noescape', 'lifetime_capture_by'),
  /// so the analysis cannot tell whether the borrow escapes the call.
  UnannotatedIndirection,
  /// An ownership-transferring move of an owner (e.g. 'std::move(owner)' or
  /// 'std::unique_ptr::release'), which the analysis does not model, silencing
  /// lifetime checks for the moved-from object.
  MoveSilencing,
  /// An expression or declaration of a user-defined type whose ownership is
  /// unknown (it can hold a borrow but is annotated neither [[gsl::Owner]] nor
  /// [[gsl::Pointer]]), e.g. a call returning such a type or a local of one.
  UnknownOwnership,
  /// A `throw` or `try`/`catch`. Exception control flow (unwinding, running
  /// destructors and resuming in a handler) is not modeled, so lifetime errors
  /// on exception paths may be missed.
  Exception,
  /// A value of a [[gsl::Owner]] container whose element type is an indirection
  /// (e.g. std::vector<int*>). Per-element borrows are not tracked.
  OwnerOfIndirection,
  /// A value of a [[gsl::Pointer]] view whose pointee/element type is itself an
  /// indirection (e.g. std::span<int*>). The view hands out borrows one level
  /// deeper than it tracks; those inner pointees are not modeled.
  PointerOfIndirection,
  /// A view (gsl::Pointer) constructed from a mutable global/static owner (e.g.
  /// 'std::string_view sv = some_global_string;'). The global can be mutated
  /// from anywhere -- including other functions or translation units the
  /// intra-procedural analysis cannot see -- invalidating the view, so the
  /// borrow cannot be tracked.
  ViewOnMutableGlobal,
  /// A `const` member function that mutates an owner reached through the pointee
  /// of an owning smart-pointer data member (e.g. `void f() const {
  /// uptr_->append(...); }` where `uptr_` is `std::unique_ptr<std::string>`).
  /// `const` does not propagate through a smart pointer, so the pointee is
  /// mutable; this subverts the analysis's assumption that a const member
  /// function does not invalidate borrows into the object (it can dangle a view
  /// returned by a sibling accessor).
  ConstMethodIndirectMutation,
  /// An expression that forms more than one level of indirection, e.g. `&p`
  /// where `p` is itself a pointer/view (taking the address of an indirection),
  /// or constructing a view over a pointer/view. The declaration-level
  /// single-indirection rule is mirrored here for transient expressions, which
  /// could otherwise build a double indirection no declaration captures.
  MultiLevelIndirectionExpr,
  /// A member access on a union. Different union members alias the same storage,
  /// so a borrow into one member can be invalidated by writing another (or by
  /// switching the active member); the analysis keys borrows by field identity
  /// and does not model this aliasing, so such a borrow can dangle undetected.
  Union,
  /// A `reinterpret_cast`. It can launder a borrow through an unrelated type
  /// (reinterpreting storage), hiding its provenance from the analysis, so a
  /// borrow recovered through it is not tracked.
  ReinterpretCast,
  /// A by-reference lambda capture of an indirection-typed variable (e.g. `[&sv]`
  /// where sv is a std::string_view): the capture forms a reference to a view --
  /// two levels of indirection -- and a reassignment of the view inside the
  /// lambda body is not flowed back, so a borrow it holds can dangle undetected.
  LambdaRefCaptureIndirection,
  /// An array whose element type is itself an indirection (a pointer, reference,
  /// or view) decaying to a pointer (e.g. `int* arr[N]` -> `int**`), other than
  /// as the base of an `arr[i]` subscript. The decay forms a pointer-to-pointer
  /// -- a double indirection the analysis cannot model (like `int**` / `&p`), and
  /// per-element borrows of such an array cannot be tracked (cf. std::vector<int*>).
  ArrayOfIndirectionDecay,
  /// An assignment whose destination lvalue selects/forwards among several
  /// objects -- e.g. `(c ? p : q) = ...`, `(f(), p) = ...`, or those wrapped in
  /// `*&(...)`/casts -- so a stored borrow cannot be routed to a tracked storage
  /// origin and is dropped. The destination is borrow-holding (a pointer/view).
  UnsupportedStoreDestination,
  /// Soundness catch-all: an expression whose type carries a borrow (a pointer,
  /// reference, or view) for which the fact generator has no specific handler --
  /// e.g. a C11 atomic builtin (`AtomicExpr`) or a compound literal. Such an
  /// expression produces a value whose origin is never populated, so any borrow
  /// it should carry is silently dropped. Flagging it keeps the soundness model
  /// from silently failing on a construct it does not model.
  UnmodeledExpr,
  /// An inline assembly statement (`asm(...)`). The analysis does not model what
  /// the asm does: an output operand can reseat a pointer to anything, and an
  /// input/memory-clobbering operand can move or invalidate a borrow, with no
  /// visible flow. A stale loan on an asm-reseated pointer would otherwise be
  /// trusted. The construct is rejected under the safe programming model.
  InlineAsm,
  /// A call to a `setjmp`/`longjmp` family function (recognized via the
  /// `returns_twice` attribute or the setjmp/longjmp builtins). Non-local
  /// control flow -- `longjmp` transferring back to a `setjmp` point, which the
  /// CFG does not model as a back-edge -- can re-enter scopes and re-run code in
  /// a way the dataflow cannot follow, so a borrow invalidated before the jump
  /// and used after it would be missed. The construct is rejected under the safe
  /// programming model.
  SetjmpLongjmp,
  /// A coroutine. Its body is deferred past suspension points and resumed later,
  /// possibly after a by-reference argument's temporary has been destroyed (the
  /// coroutine frame outlives the call's full-expression). The analysis models
  /// the call as ordinary, so a borrowed parameter used in the resumed body --
  /// after its argument died -- is not connected to that expiry and would be
  /// missed. The construct is rejected under the safe programming model.
  Coroutine,
};

/// Records a construct that the analysis cannot fully model, attached to the
/// CFG block where it appears. The checker turns these into soundness
/// diagnostics. This fact carries no Origin/Loan state and is ignored by the
/// dataflow analyses.
class UntrackedConstructFact : public Fact {
  UntrackedConstructReason Reason;
  /// The expression the diagnostic should point at (for location and range).
  /// Null when the construct is a declaration (see ConstructDecl).
  const Expr *ConstructExpr;
  /// The declaration the diagnostic should point at, when the construct is a
  /// declaration rather than an expression. Null otherwise.
  const ValueDecl *ConstructDecl;
  /// The location the diagnostic should point at, when the construct is a
  /// statement that is neither an expression nor a declaration (e.g. a `try`).
  /// Invalid otherwise.
  SourceLocation ConstructLoc;
  /// For the owner-/pointer-of-indirection reasons, the precise borrow-holding
  /// type to name in the diagnostic, when it differs from the construct's own
  /// type -- e.g. the std::vector<std::string_view> element buried in a
  /// std::pair<std::vector<std::string_view>, int> local. Null when the
  /// construct's own type is the offending type (the common case).
  QualType ReportType;

public:
  static bool classof(const Fact *F) {
    return F->getKind() == Kind::UntrackedConstruct;
  }

  UntrackedConstructFact(UntrackedConstructReason Reason, const Expr *E,
                         QualType ReportType = QualType())
      : Fact(Kind::UntrackedConstruct), Reason(Reason), ConstructExpr(E),
        ConstructDecl(nullptr), ReportType(ReportType) {}
  UntrackedConstructFact(UntrackedConstructReason Reason, const ValueDecl *D,
                         QualType ReportType = QualType())
      : Fact(Kind::UntrackedConstruct), Reason(Reason), ConstructExpr(nullptr),
        ConstructDecl(D), ReportType(ReportType) {}
  UntrackedConstructFact(UntrackedConstructReason Reason, SourceLocation Loc)
      : Fact(Kind::UntrackedConstruct), Reason(Reason), ConstructExpr(nullptr),
        ConstructDecl(nullptr), ConstructLoc(Loc) {}

  UntrackedConstructReason getReason() const { return Reason; }
  const Expr *getConstructExpr() const { return ConstructExpr; }
  const ValueDecl *getConstructDecl() const { return ConstructDecl; }
  SourceLocation getConstructLoc() const { return ConstructLoc; }
  QualType getReportType() const { return ReportType; }

  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &) const override;
};

/// A borrow stored into a view/pointer data member `obj.field = ...`. Records
/// the stored value's origin and the origin of the destination's enclosing
/// object, so the checker can flag a self-referential object: one where the
/// stored value borrows the same object that holds the member (they share an
/// object-identity loan). Carries no dataflow state.
class FieldStoreFact : public Fact {
  /// The destination member expression (`obj.field`), for diagnostics.
  const Expr *StoreExpr;
  /// The origin whose loans are the value being stored into the member.
  OriginID StoredOrigin;
  /// The origin of the member's enclosing object (`obj` / `this`).
  OriginID ContainerOrigin;

public:
  static bool classof(const Fact *F) {
    return F->getKind() == Kind::FieldStore;
  }

  FieldStoreFact(const Expr *StoreExpr, OriginID StoredOrigin,
                 OriginID ContainerOrigin)
      : Fact(Kind::FieldStore), StoreExpr(StoreExpr), StoredOrigin(StoredOrigin),
        ContainerOrigin(ContainerOrigin) {}

  const Expr *getStoreExpr() const { return StoreExpr; }
  OriginID getStoredOrigin() const { return StoredOrigin; }
  OriginID getContainerOrigin() const { return ContainerOrigin; }

  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

/// One argument to a call that the call may mutate (`MutatingOrigin` -- an owner
/// passed by non-const reference/pointer, or the receiver of a mutating method),
/// together with the other borrow-holding arguments to the same call
/// (`BorrowOrigins`). The checker flags any of those that alias the mutated one
/// (shares a loan): the call may reallocate the owner and invalidate the
/// co-argument's borrow, which no lifetime annotation expresses. Carries no
/// dataflow state.
class ArgOverlapFact : public Fact {
  /// The call expression, for diagnostics.
  const Expr *OperationExpr;
  /// The argument the call may mutate, together with its pointee chain: for a
  /// `gsl::Pointer` receiver (e.g. a wrapper holding `Owner* p`) the aliased
  /// owner's borrow lives one indirection level in, on the pointee origin, not
  /// on the wrapper's own origin -- so the checker must union loans across all
  /// of them (allocated in the FactManager's allocator).
  llvm::ArrayRef<OriginID> MutatingOrigins;
  /// The static record type of the mutated argument -- the actual subobject
  /// being mutated (e.g. `Grid` for `world.grid_.build(...)`), or null if the
  /// mutated argument is not a record. Used for the subobject-containment check:
  /// the mutating origin's loan may widen to the enclosing object's placeholder
  /// (`$this`), so deriving the mutated record from the loan would over-match
  /// disjoint sibling fields. The precise static type does not.
  const CXXRecordDecl *MutatedRecord;
  /// The other borrow-holding arguments to the same call (allocated in the
  /// FactManager's allocator).
  llvm::ArrayRef<OriginID> BorrowOrigins;

public:
  static bool classof(const Fact *F) {
    return F->getKind() == Kind::ArgumentOverlap;
  }

  ArgOverlapFact(const Expr *OperationExpr,
                 llvm::ArrayRef<OriginID> MutatingOrigins,
                 const CXXRecordDecl *MutatedRecord,
                 llvm::ArrayRef<OriginID> BorrowOrigins)
      : Fact(Kind::ArgumentOverlap), OperationExpr(OperationExpr),
        MutatingOrigins(MutatingOrigins), MutatedRecord(MutatedRecord),
        BorrowOrigins(BorrowOrigins) {}

  const Expr *getOperationExpr() const { return OperationExpr; }
  llvm::ArrayRef<OriginID> getMutatingOrigins() const { return MutatingOrigins; }
  const CXXRecordDecl *getMutatedRecord() const { return MutatedRecord; }
  llvm::ArrayRef<OriginID> getBorrowOrigins() const { return BorrowOrigins; }

  void dump(llvm::raw_ostream &OS, const LoanManager &,
            const OriginManager &OM) const override;
};

class FactManager {
public:
  FactManager(const AnalysisDeclContext &AC, const CFG &Cfg) : OriginMgr(AC) {
    BlockToFacts.resize(Cfg.getNumBlockIDs());
  }

  llvm::ArrayRef<const Fact *> getFacts(const CFGBlock *B) const {
    return BlockToFacts[B->getBlockID()];
  }

  void addBlockFacts(const CFGBlock *B, llvm::ArrayRef<Fact *> NewFacts) {
    if (!NewFacts.empty())
      BlockToFacts[B->getBlockID()].assign(NewFacts.begin(), NewFacts.end());
  }

  template <typename FactType, typename... Args>
  FactType *createFact(Args &&...args) {
    void *Mem = FactAllocator.Allocate<FactType>();
    FactType *Res = new (Mem) FactType(std::forward<Args>(args)...);
    Res->setID(NextFactID++);
    return Res;
  }

  /// Copies `Elts` into the fact allocator so a fact can hold a stable
  /// reference to a variable-length list.
  template <typename T> llvm::ArrayRef<T> copyToFactStorage(ArrayRef<T> Elts) {
    return Elts.copy(FactAllocator);
  }

  void dump(const CFG &Cfg, AnalysisDeclContext &AC) const;

  /// Retrieves program points that were specially marked in the source code
  /// for testing.
  ///
  /// The analysis recognizes special function calls of the form
  /// `void("__lifetime_test_point_<name>")` as test points. This method returns
  /// a map from the annotation string (<name>) to the corresponding
  /// `ProgramPoint`. This allows test harnesses to query the analysis state at
  /// user-defined locations in the code.
  /// \note This is intended for testing only.
  llvm::StringMap<ProgramPoint> getTestPoints() const;
  /// Retrieves all the facts in the block containing Program Point P.
  /// \note This is intended for testing only.
  llvm::ArrayRef<const Fact *> getBlockContaining(ProgramPoint P) const;

  unsigned getNumFacts() const { return NextFactID.Value; }

  LoanManager &getLoanMgr() { return LoanMgr; }
  const LoanManager &getLoanMgr() const { return LoanMgr; }
  OriginManager &getOriginMgr() { return OriginMgr; }
  const OriginManager &getOriginMgr() const { return OriginMgr; }

  /// Memoizes `isUnknownOwnershipType` results (keyed by canonical type) for
  /// the duration of the analysis.
  llvm::DenseMap<const Type *, bool> &getUnknownOwnershipCache() {
    return UnknownOwnershipCache;
  }

private:
  FactID NextFactID{0};
  LoanManager LoanMgr;
  OriginManager OriginMgr;
  /// Facts for each CFG block, indexed by block ID.
  llvm::SmallVector<llvm::SmallVector<const Fact *>> BlockToFacts;
  llvm::BumpPtrAllocator FactAllocator;
  llvm::DenseMap<const Type *, bool> UnknownOwnershipCache;
};
} // namespace clang::lifetimes::internal

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_FACTS_H
