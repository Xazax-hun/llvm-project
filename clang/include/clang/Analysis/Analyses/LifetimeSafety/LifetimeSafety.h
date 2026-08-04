//===- LifetimeSafety.h - C++ Lifetime Safety Analysis -*----------- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the main entry point and orchestrator for the C++ Lifetime
// Safety Analysis. It coordinates the entire analysis pipeline: fact
// generation, loan propagation, live origins analysis, and enforcement of
// lifetime safety policy.
//
// The analysis is based on the concepts of "origins" and "loans" to track
// pointer lifetimes and detect issues like use-after-free and dangling
// pointers. See the RFC for more details:
// https://discourse.llvm.org/t/rfc-intra-procedural-lifetime-analysis-in-clang/86291
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_H

#include "clang/AST/Decl.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeStats.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LiveOrigins.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LoanPropagation.h"
#include "clang/Analysis/Analyses/LifetimeSafety/MovedLoans.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Origins.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "llvm/ADT/PointerUnion.h"
#include <cstddef>
#include <memory>

namespace clang::lifetimes {

struct LifetimeSafetyOpts {
  /// Maximum number of CFG blocks to analyze. Functions with larger CFGs will
  /// be skipped.
  size_t MaxCFGBlocks;
};

/// Enum to track functions visible across or within TU.
enum class WarningScope {
  CrossTU, // For warnings on declarations visible across Translation Units.
  IntraTU  // For warnings on functions local to a Translation Unit.
};

/// Why the analysis skipped a function entirely. The values match the
/// `%select` order of `warn_lifetime_safety_bailout`.
enum class BailoutReason : unsigned {
  CFGTooLarge = 0,   // The CFG exceeded the configured block limit.
  CFGUnavailable = 1 // The CFG could not be built for the function.
};

/// Abstract interface for operations requiring Sema access.
///
/// This class exists to break a circular dependency: the LifetimeSafety
/// analysis target cannot directly depend on clangSema (which would create the
/// cycle: clangSema -> clangAnalysis -> clangAnalysisLifetimeSafety ->
/// clangSema).
///
/// Instead, this interface is implemented in AnalysisBasedWarnings.cpp (part of
/// clangSema), allowing the analysis to report diagnostics and modify the AST
/// through Sema without introducing a circular dependency.
class LifetimeSafetySemaHelper {
public:
  LifetimeSafetySemaHelper() = default;
  virtual ~LifetimeSafetySemaHelper() = default;

  virtual void reportUseAfterScope(const Expr *IssueExpr, const Expr *UseExpr,
                                   const Expr *MovedExpr,
                                   SourceLocation FreeLoc) {}
  // Variant for an implicit use (a non-trivial destructor reading a borrow at
  // scope exit) that has no source expression; `UseLoc` anchors the "used here"
  // note.
  virtual void reportUseAfterScope(const Expr *IssueExpr, SourceLocation UseLoc,
                                   const Expr *MovedExpr,
                                   SourceLocation FreeLoc) {}

  virtual void reportUseAfterReturn(const Expr *IssueExpr,
                                    const Expr *ReturnExpr,
                                    const Expr *MovedExpr) {}

  virtual void reportDanglingField(const Expr *IssueExpr,
                                   const FieldDecl *Field,
                                   const Expr *MovedExpr,
                                   SourceLocation ExpiryLoc) {}

  virtual void reportDanglingGlobal(const Expr *IssueExpr,
                                    const VarDecl *DanglingGlobal,
                                    const Expr *MovedExpr,
                                    SourceLocation ExpiryLoc) {}

  // Reports when a reference/iterator is used after the container operation
  // that invalidated it.
  virtual void reportUseAfterInvalidation(const Expr *IssueExpr,
                                          const Expr *UseExpr,
                                          const Expr *InvalidationExpr) {}
  virtual void reportUseAfterInvalidation(const ParmVarDecl *PVD,
                                          const Expr *UseExpr,
                                          const Expr *InvalidationExpr) {}
  // Variants for an implicit use (a non-trivial destructor) with no source
  // expression; `UseLoc` anchors the "used here" note.
  virtual void reportUseAfterInvalidation(const Expr *IssueExpr,
                                          SourceLocation UseLoc,
                                          const Expr *InvalidationExpr) {}
  virtual void reportUseAfterInvalidation(const ParmVarDecl *PVD,
                                          SourceLocation UseLoc,
                                          const Expr *InvalidationExpr) {}
  virtual void reportInvalidatedField(const Expr *IssueExpr,
                                      const FieldDecl *Field,
                                      const Expr *InvalidationExpr) {}
  virtual void reportInvalidatedField(const ParmVarDecl *PVD,
                                      const FieldDecl *Field,
                                      const Expr *InvalidationExpr) {}
  virtual void reportInvalidatedGlobal(const Expr *IssueExpr,
                                       const VarDecl *Global,
                                       const Expr *InvalidationExpr) {}
  virtual void reportInvalidatedGlobal(const ParmVarDecl *PVD,
                                       const VarDecl *Global,
                                       const Expr *InvalidationExpr) {}

  using EscapingTarget =
      llvm::PointerUnion<const Expr *, const FieldDecl *, const VarDecl *>;

  // Suggests lifetime bound annotations for function parameters.
  virtual void suggestLifetimeboundToParmVar(WarningScope Scope,
                                             const ParmVarDecl *ParmToAnnotate,
                                             EscapingTarget Target) {}

  // Reports misuse of [[clang::noescape]] when parameter escapes through return
  virtual void reportNoescapeViolation(const ParmVarDecl *ParmWithNoescape,
                                       const Expr *EscapeExpr) {}
  // Reports misuse of [[clang::noescape]] when parameter escapes through field
  virtual void reportNoescapeViolation(const ParmVarDecl *ParmWithNoescape,
                                       const FieldDecl *EscapeField) {}
  // Reports misuse of [[clang::noescape]] when parameter escapes through
  // assignment to a global variable
  virtual void reportNoescapeViolation(const ParmVarDecl *ParmWithNoescape,
                                       const VarDecl *EscapeGlobal) {}

  // Reports misuse of [[clang::lifetimebound]] when parameter doesn't escape
  // through return.
  virtual void
  reportLifetimeboundViolation(const ParmVarDecl *ParmWithLifetimebound) {}

  // Reports misuse of [[clang::lifetimebound]] when implicit this parameter
  // doesn't escape through return.
  virtual void
  reportLifetimeboundViolation(const CXXMethodDecl *MDWithLifetimebound) {}

  // Reports a [[clang::lifetime_immortal]] function whose body returns a borrow
  // of non-immortal storage. `Subject` selects the borrowed entity: 0 = a
  // local/temporary, 1 = a parameter, 2 = the implicit this parameter.
  virtual void reportImmortalViolation(const FunctionDecl *FD,
                                       unsigned Subject) {}

  // Reports a member function definition that has [[clang::lifetimebound]] on
  // the implicit this parameter when the canonical declaration does not.
  virtual void reportMisplacedLifetimebound(WarningScope Scope,
                                            const CXXMethodDecl *FDef,
                                            const CXXMethodDecl *FDecl) {}

  // Reports a function definition parameter that has [[clang::lifetimebound]]
  // when the corresponding parameter in the canonical declaration.
  virtual void reportMisplacedLifetimebound(WarningScope Scope,
                                            const ParmVarDecl *PVDDef,
                                            const ParmVarDecl *PVDDecl) {}

  // Suggests lifetime bound annotations for implicit this.
  virtual void suggestLifetimeboundToImplicitThis(WarningScope Scope,
                                                  const CXXMethodDecl *MD,
                                                  const Expr *EscapeExpr) {}

  // Adds inferred lifetime bound attribute for implicit this to its
  // TypeSourceInfo.
  virtual void addLifetimeBoundToImplicitThis(const CXXMethodDecl *MD) {}

  // Soundness ("safe programming model") diagnostics. These fire wherever the
  // analysis cannot fully model a construct; enabling them as errors over a
  // region guarantees no lifetime mistake slips through unmodeled.

  // Reports a read of a pointer-like value that the analysis tracks but for
  // which no borrow information remains, indicating a loan was lost to an
  // unmodeled construct during loan propagation.
  virtual void reportLostLoan(const Expr *UseExpr) {}

  // Reports that the analysis skipped a function entirely (e.g. its CFG exceeds
  // the configured block limit, or the CFG could not be built), so lifetime
  // mistakes in it may go undetected.
  virtual void reportAnalysisBailout(const Decl *FD, BailoutReason Reason) {}

  // Reports a call whose callee could not be resolved to a function (e.g. a
  // call through a function or member-function pointer), which the analysis
  // cannot model.
  virtual void reportIndirectCall(const Expr *CallExpr) {}

  // Reports an argument bound to a pointer/reference parameter that carries no
  // lifetime annotation, so the analysis cannot tell whether the borrow escapes.
  virtual void reportUnannotatedIndirection(const Expr *ArgExpr) {}

  // Reports a pointer/reference parameter of the analyzed function that carries
  // no lifetime annotation, leaving its lifetime contract unspecified.
  virtual void reportUnannotatedParam(const ParmVarDecl *PVD) {}

  // Reports an instance member function (including a conversion operator) whose
  // return type is an indirection (pointer, reference, or gsl::Pointer) but
  // whose implicit object is not [[clang::lifetimebound]] and which is not
  // [[clang::lifetime_immortal]], leaving the returned borrow's lifetime
  // unspecified for callers.
  virtual void reportUnannotatedThisReturn(const CXXMethodDecl *MD) {}

  // Reports a borrow of the implicit object ('this', \p IsField false) or one of
  // its fields (\p IsField true) escaping to global or static storage \p Global
  // from a method. The global outlives the call, but the object's lifetime is
  // caller-determined, so the stored borrow may dangle.
  virtual void reportThisEscapesToGlobal(SourceLocation Loc, bool IsField,
                                         const VarDecl *Global) {}

  // Reports a parameter annotated [[clang::lifetimebound]] or
  // [[clang::lifetime_capture_by]] (to an entity other than `global`) whose
  // borrow escapes to global or static storage \p Global. Those annotations
  // describe a return/capture relationship, not a global capture, so the
  // escape is uncovered and the caller is unaware the global aliases the
  // argument.
  virtual void reportAnnotatedParamEscapesToGlobal(const ParmVarDecl *PVD,
                                                   const VarDecl *Global) {}

  // Reports a parameter annotated [[clang::lifetime_capture_by(X)]] (with X not
  // naming `this`) whose borrow is nonetheless captured into the enclosing
  // object -- stored into a field of `this`. The annotation names a different
  // capturer than the body uses, so the analysis trusted the wrong entity and
  // suppressed the unannotated-indirection backstop; the real capture into
  // `this` went unchecked and the borrow can dangle. Annotate the parameter
  // [[clang::lifetime_capture_by(this)]] (or [[clang::lifetimebound]]) to match
  // the body.
  virtual void reportCaptureByViolation(const ParmVarDecl *PVD) {}

  // Reports a parameter annotated [[clang::lifetime_capture_by(global)]] or
  // [[clang::lifetime_capture_by(unknown)]]. The safe programming model rejects
  // both: the analysis cannot track a borrow captured into global/static storage
  // or an unspecified location, so such a capture may dangle undetected.
  // \p IsUnknown selects which spelling is reported.
  virtual void reportGlobalCapture(const ParmVarDecl *PVD, bool IsUnknown) {}

  // Reports a declaration whose type uses more than one level of indirection
  // (e.g. 'int **'), which the analysis cannot fully model.
  virtual void reportMultiLevelIndirection(const ValueDecl *VD) {}

  // Reports an expression that forms more than one level of indirection (e.g.
  // '&p' where 'p' is itself a pointer/view), which the analysis cannot fully
  // model. Mirrors the declaration-level ban for transient expressions.
  virtual void reportMultiLevelIndirection(const Expr *E) {}

  // Reports a function whose RETURN TYPE uses more than one level of indirection
  // (e.g. 'std::string_view&' -- a reference to a view). Storing through such a
  // returned reference silently drops the borrow, so the pattern is rejected.
  virtual void reportMultiLevelIndirectionReturn(const FunctionDecl *FD) {}

  // Reports a by-reference lambda capture of an indirection-typed variable (e.g.
  // capturing a std::string_view by `[&sv]`), which forms a reference to a view
  // -- two levels of indirection. A reassignment of the view inside the lambda
  // body is not modeled back into the captured variable, so a borrow held by it
  // can dangle undetected. \p CaptureRef is the capture's reference to the
  // variable (for the name and location).
  virtual void reportMultiLevelIndirectionCapture(const Expr *CaptureRef) {}

  // Reports an array of indirections (`int* arr[N]`, `std::string_view arr[N]`)
  // decaying to a pointer-to-pointer somewhere other than an `arr[i]` subscript
  // base -- a double level of indirection the analysis cannot model.
  virtual void reportArrayOfIndirectionDecay(const Expr *E) {}

  // Reports an assignment whose destination lvalue selects/forwards among
  // several objects (`(c ? p : q) = ...`, `(f(), p) = ...`, `*&(...) = ...`), so
  // a stored borrow cannot be routed to a tracked storage origin.
  virtual void reportUnsupportedStoreDestination(const Expr *E) {}

  // Reports an origin-bearing expression the fact generator does not model (no
  // specific handler), so any borrow it carries is silently dropped.
  virtual void reportUnmodeledExpr(const Expr *E) {}

  // Reports an ownership-transferring move of an owner (e.g. 'std::move' of a
  // gsl::Owner, or 'unique_ptr::release'), which the analysis does not model.
  virtual void reportMoveSilencing(const Expr *MoveExpr) {}

  // Reports a borrow that may be invalidated by an operation the analysis
  // conservatively assumes mutates the owner (a non-const member call, or
  // passing the owner to a non-const pointer/reference parameter).
  virtual void reportAssumedInvalidation(const Expr *IssueExpr,
                                         const Stmt *OperationStmt) {}
  virtual void reportAssumedInvalidation(const ParmVarDecl *PVD,
                                         const Stmt *OperationStmt) {}
  // Same, for a borrow of the implicit object (`this`) passed as an aliasing
  // argument to the operation; anchored at the method it belongs to.
  virtual void reportAssumedInvalidation(const CXXMethodDecl *MD,
                                         const Stmt *OperationStmt) {}

  // Reports a 'delete'/'free' of a pointer whose allocation the analysis did not
  // see, so it cannot verify the deallocation is a live, unaliased allocation.
  virtual void reportNakedDeallocation(const Expr *DeallocExpr) {}

  // Reports a declaration of a user-defined type that can hold a borrow but is
  // annotated neither [[gsl::Owner]] nor [[gsl::Pointer]].
  virtual void reportUnknownOwnership(const ValueDecl *VD) {}
  // Reports an expression (e.g. a call result) of such a type.
  virtual void reportUnknownOwnership(const Expr *E) {}

  // Reports a `throw` or `try`/`catch`. Exception control flow (stack
  // unwinding, running destructors and resuming in a handler) is not modeled,
  // so a borrow that dangles only along an exception path can be missed.
  virtual void reportException(SourceLocation Loc) {}

  // Reports an inline assembly statement. The analysis cannot model what the asm
  // does -- an output operand can reseat a pointer, and an input/memory clobber
  // can move or invalidate a borrow -- so it is rejected.
  virtual void reportInlineAsm(SourceLocation Loc) {}

  // Reports a setjmp/longjmp family call. Its non-local control flow (a jump back
  // to the setjmp point) is not modeled by the CFG, so a borrow invalidated
  // before the jump and used after it re-enters could be missed.
  virtual void reportSetjmpLongjmp(SourceLocation Loc) {}

  // Reports a coroutine. Its body is deferred past suspension points and resumed
  // later, possibly after a by-reference argument's temporary has been
  // destroyed, so a dangling use in the resumed body could be missed.
  virtual void reportCoroutine(SourceLocation Loc) {}

  // Reports a member access on a union. Different union members alias the same
  // storage, so a borrow into one member can be invalidated by writing another;
  // the analysis keys borrows by field identity and does not model this.
  virtual void reportUnion(const Expr *E) {}

  // Reports a `reinterpret_cast`, which can launder a borrow through an
  // unrelated type and hide its provenance from the analysis.
  virtual void reportReinterpretCast(const Expr *E) {}

  // Reports a value of a [[gsl::Owner]] container whose element type is an
  // indirection (e.g. std::vector<int*>); per-element borrows are not tracked.
  // \p ReportType, when non-null, is the precise borrow-holding type to name
  // (e.g. the element buried in a std::pair argument); otherwise the construct's
  // own type is used.
  virtual void reportOwnerOfIndirection(const ValueDecl *VD,
                                        QualType ReportType = QualType()) {}
  virtual void reportOwnerOfIndirection(const Expr *E,
                                        QualType ReportType = QualType()) {}

  // Reports a value of a [[gsl::Pointer]] view whose pointee/element type is
  // itself an indirection (e.g. std::span<int*>); the inner pointees are not
  // tracked. \p ReportType as above.
  virtual void reportPointerOfIndirection(const ValueDecl *VD,
                                          QualType ReportType = QualType()) {}
  virtual void reportPointerOfIndirection(const Expr *E,
                                          QualType ReportType = QualType()) {}

  // Reports a view (gsl::Pointer) constructed from a mutable global/static owner
  // (e.g. 'std::string_view sv = some_global_string;'). The global can be
  // mutated elsewhere -- invalidating the view -- which the intra-procedural
  // analysis cannot track.
  virtual void reportViewOnMutableGlobal(SourceLocation Loc, QualType ViewTy,
                                         SourceRange Range) {}

  // Reports a borrow of a global/static owner (even a `const` one) that has a
  // non-trivial destructor and that escapes into other global/static storage.
  // The borrowed storage is freed at static destruction, whose order across
  // translation units the intra-procedural analysis cannot track, so a
  // longer-lived global holding the borrow can read freed memory at teardown.
  virtual void reportGlobalDtorOrder(SourceLocation Loc, QualType ViewTy,
                                     SourceRange Range) {}

  // Reports a `const` member function that mutates an owner reached through the
  // pointee of an owning smart-pointer data member. `const` does not protect the
  // pointee, so this can invalidate borrows the analysis assumes a const member
  // function leaves intact.
  virtual void reportConstMethodIndirectMutation(const Expr *E) {}

  // Reports a const member function that hands out (returns / stores) a
  // non-const pointer or reference into a mutable owner reached from the
  // object. The caller can mutate the owner through the escaped indirection,
  // which can invalidate borrows the analysis assumes a const member function
  // leaves intact. This is the escape-site counterpart of
  // reportConstMethodIndirectMutation.
  virtual void reportConstMethodIndirectEscape(const Expr *E) {}

  // Reports a self-referential borrow: a view/pointer member bound to a sibling
  // member of the same object (e.g. 'this->view = this->str;'). Mutating or
  // moving the object invalidates the view, which the intra-procedural analysis
  // cannot track across calls.
  virtual void reportSelfReferentialBorrow(const Expr *E) {}
};

/// The main entry point for the analysis.
void runLifetimeSafetyAnalysis(AnalysisDeclContext &AC,
                               LifetimeSafetySemaHelper *SemaHelper,
                               LifetimeSafetyStats &Stats, bool CollectStats);

namespace internal {

void collectLifetimeStats(AnalysisDeclContext &AC, OriginManager &OM,
                          LifetimeSafetyStats &Stats);

/// An object to hold the factories for immutable collections, ensuring
/// that all created states share the same underlying memory management.
struct LifetimeFactory {
  OriginLoanMap::Factory OriginMapFactory{/*canonicalize=*/false};
  LoanSet::Factory LoanSetFactory{/*canonicalize=*/false};
  MovedLoansMap::Factory MovedLoansMapFactory{/*canonicalize=*/false};
  LivenessMap::Factory LivenessMapFactory{/*canonicalize=*/false};
};

/// Running the lifetime safety analysis and querying its results. It
/// encapsulates the various dataflow analyses.
class LifetimeSafetyAnalysis {
public:
  LifetimeSafetyAnalysis(AnalysisDeclContext &AC,
                         LifetimeSafetySemaHelper *SemaHelper,
                         const LifetimeSafetyOpts &LSOpts);

  void run();

  /// \note These are provided only for testing purposes.
  LoanPropagationAnalysis &getLoanPropagation() const {
    return *LoanPropagation;
  }
  LiveOriginsAnalysis &getLiveOrigins() const { return *LiveOrigins; }
  FactManager &getFactManager() { return *FactMgr; }

private:
  AnalysisDeclContext &AC;
  LifetimeSafetySemaHelper *SemaHelper;
  const LifetimeSafetyOpts LSOpts;
  LifetimeFactory Factory;
  std::unique_ptr<FactManager> FactMgr;
  std::unique_ptr<LiveOriginsAnalysis> LiveOrigins;
  std::unique_ptr<LoanPropagationAnalysis> LoanPropagation;
  std::unique_ptr<MovedLoansAnalysis> MovedLoans;
};
} // namespace internal
} // namespace clang::lifetimes

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_H
