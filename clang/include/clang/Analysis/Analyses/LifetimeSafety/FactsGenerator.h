//===- FactsGenerator.h - Lifetime Facts Generation -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the FactsGenerator, which traverses the AST to generate
// lifetime-relevant facts (such as loan issuance, expiration, origin flow,
// and use) from CFG statements. These facts are used by the dataflow analyses
// to track pointer lifetimes and detect use-after-free errors.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_FACTSGENERATOR_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_FACTSGENERATOR_H

#include "clang/AST/StmtVisitor.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Origins.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace clang::lifetimes::internal {

class FactsGenerator : public ConstStmtVisitor<FactsGenerator> {
  using Base = ConstStmtVisitor<FactsGenerator>;

public:
  FactsGenerator(FactManager &FactMgr, AnalysisDeclContext &AC)
      : FactMgr(FactMgr), AC(AC) {}

  void run();

  void VisitDeclStmt(const DeclStmt *DS);
  /// Soundness catch-all: the fallback for any expression with no specific
  /// handler. Flags an origin-bearing expression the fact generator does not
  /// model (so its borrow would be silently dropped).
  void VisitExpr(const Expr *E);
  void VisitDeclRefExpr(const DeclRefExpr *DRE);
  void VisitCXXConstructExpr(const CXXConstructExpr *CCE);
  void VisitCXXDefaultInitExpr(const CXXDefaultInitExpr *DIE);
  /// Replays the "this construct is not modeled" refusals for an expression the CFG
  /// does not descend into, so they are not lost where no Visit method ever runs.
  void VisitCXXMemberCallExpr(const CXXMemberCallExpr *MCE);
  void VisitMemberExpr(const MemberExpr *ME);
  void VisitCallExpr(const CallExpr *CE);
  void VisitCXXNullPtrLiteralExpr(const CXXNullPtrLiteralExpr *N);
  void VisitCastExpr(const CastExpr *CE);
  void VisitUnaryOperator(const UnaryOperator *UO);
  void VisitReturnStmt(const ReturnStmt *RS);
  void VisitBinaryOperator(const BinaryOperator *BO);
  void VisitConditionalOperator(const ConditionalOperator *CO);
  void
  VisitBinaryConditionalOperator(const BinaryConditionalOperator *BCO);
  void VisitCXXOperatorCallExpr(const CXXOperatorCallExpr *OCE);
  void VisitCXXFunctionalCastExpr(const CXXFunctionalCastExpr *FCE);
  void VisitInitListExpr(const InitListExpr *ILE);
  void VisitCXXParenListInitExpr(const CXXParenListInitExpr *PLIE);
  /// Models aggregate initialization of a [[gsl::Pointer]] / [[gsl::Owner]]
  /// leaf record (`View{heap}`, `View{.p = local}`, `View(p)`) by merging each
  /// borrow-carrying initializer's loans into the aggregate's own origin.
  void handleGslAggregateInit(const Expr *AggExpr,
                              llvm::ArrayRef<const Expr *> Inits);
  /// Soundness: a plain (non-gsl) aggregate `AggExpr` that can hold a borrow but
  /// whose ownership is untracked. A local/member declaration of such a type is
  /// reported at the declaration (VisitDeclStmt) and a call result at the call,
  /// but an aggregate *temporary* that escapes (`return Box{&x}`, `g =
  /// Box{&x}.p`) is covered by neither -- its borrow is orphaned and silently
  /// dropped. Emits an UntrackedConstructFact for the escaping temporary form;
  /// skips an aggregate that directly initializes a declaration (already
  /// reported there) to avoid double-reporting.
  void maybeReportUntrackedAggregateTemporary(const Expr *AggExpr);
  void VisitCXXBindTemporaryExpr(const CXXBindTemporaryExpr *BTE);
  void VisitMaterializeTemporaryExpr(const MaterializeTemporaryExpr *MTE);
  void VisitLambdaExpr(const LambdaExpr *LE);
  void VisitArraySubscriptExpr(const ArraySubscriptExpr *ASE);
  void VisitCXXNewExpr(const CXXNewExpr *NE);
  void VisitCXXDeleteExpr(const CXXDeleteExpr *DE);
  void VisitCXXThrowExpr(const CXXThrowExpr *TE);
  void VisitStmtExpr(const StmtExpr *SE);
  void VisitGCCAsmStmt(const GCCAsmStmt *AS);
  void VisitMSAsmStmt(const MSAsmStmt *AS);
  void VisitOMPExecutableDirective(const OMPExecutableDirective *D);

private:
  OriginNode *getOriginNode(const ValueDecl &D);
  OriginNode *getOriginNode(const Expr &E);

  bool hasOrigins(QualType QT) const;
  bool hasOrigins(const Expr *E) const;

  void flow(OriginNode *Dst, OriginNode *Src, bool Kill,
            const CFGBlock *Block = nullptr);

  /// Flows each arm of a conditional operator into its result, path-isolated
  /// into the arm's own predecessor block.
  void handleConditionalArms(const Expr &CO, const Expr *TrueExpr,
                             const Expr *FalseExpr);

  /// Emits a single-level (top-level only) origin flow Dst <- Src, and then, if
  /// Dst carries deeper levels of indirection (a pointee chain) that the shallow
  /// flow leaves unpopulated, seeds those deeper origins with an Unknown loan
  /// (anchored at \p LoanExpr).
  ///
  /// Used where a flow is intentionally single-level -- e.g. a
  /// [[clang::lifetimebound]] return constrains only the relationship between
  /// the returned handle and the arguments at the top level -- but the
  /// destination type can be a multi-level indirection (e.g. a `const View&`
  /// return: a reference *to* a view, whose inner level is the view's own
  /// borrow). Without the Unknown loan the inner level stays empty and a use of
  /// the inner borrow is only caught by the empty-set lost-loan heuristic, which
  /// a control-flow merge can mask. The Unknown loan survives dataflow joins, so
  /// the inner borrow reliably trips lost-loan instead.
  void flowSingleLevelWithUnknownDepth(OriginNode *Dst, OriginNode *Src,
                                       const Expr *LoanExpr, bool Kill);

  /// Handles assignment for both BinaryOperator and CXXOperatorCallExpr.
  ///
  /// LHSExpr is the destination whose stored loans are replaced by RHSExpr's
  /// loans. TargetExpr is the assignment expression itself; it receives
  /// LHSExpr's origins so chained assignments like `a = b = c` can propagate
  /// through the result of `b = c`.
  void handleAssignment(const Expr *TargetExpr, const Expr *LHSExpr,
                        const Expr *RHSExpr);

  void handlePointerArithmetic(const BinaryOperator *BO);

  /// Models a non-allocating placement-new (`new (buf, ...) T`) by forwarding
  /// the placement buffer's loan to the result. Returns true if it forwarded
  /// (i.e. the operator-new is the standard non-allocating form whose first
  /// placement parameter is `void*`); false otherwise (an allocating placement
  /// form such as nothrow-new, which the caller handles as a fresh allocation).
  bool handlePlacementNew(const CXXNewExpr *NE, OriginNode *NewNode);

  void handleCXXCtorInitializer(const CXXCtorInitializer *CII);

  void handleLifetimeEnds(const CFGLifetimeEnds &LifetimeEnds);
  void handleDestructionOfBorrowHolder(QualType Ty, OriginNode *Node,
                                       const Stmt *Trigger, SourceLocation Loc);
  /// Models a member's destructor, which runs after the enclosing destructor's
  /// body, as a use of that member.
  void handleMemberDtor(const CFGMemberDtor &MemberDtor);
  void handleBaseDtor(const CFGBaseDtor &BaseDtor);

  void handleCleanupFunction(const CFGCleanupFunction &CleanupFunction);

  void handleFullExprCleanup(const CFGFullExprCleanup &FullExprCleanup);

  /// Models the destruction of a temporary whose value is discarded, which has
  /// no MaterializeTemporaryExpr and so is not covered by handleFullExprCleanup.
  void handleTemporaryDtor(const CFGTemporaryDtor &TemporaryDtor);

  /// Generates origin flows for a structured binding's holding expressions
  /// (e.g. `e[i]` / `e.field`), which are not visited by the CFG walk, so that
  /// references to the bindings -- which alias them -- are tracked. The holding
  /// variable is borrowable storage only for by-value bindings (a copy); for
  /// by-reference bindings it aliases the original.
  void handleStructuredBinding(const DecompositionDecl *DD);
  void visitBindingHoldingExpr(const Stmt *S);

  /// Scans the function body for `try`/`catch` statements and emits an
  /// Exception UntrackedConstructFact for each. A `try` whose body only calls
  /// potentially-throwing functions has no explicit `throw` and produces no
  /// CFG exception edges, so it is not reached by the CFG walk; detect it on
  /// the AST instead. (`throw` expressions are caught by VisitCXXThrowExpr.)
  void handleTryStatements();

  void handleExitBlock();

  /// Mark all fields of the implicit object as used for an instance method
  /// call, since the callee may access any part of the object.
  void handleImplicitObjectFieldUses(const Expr *Call, const FunctionDecl *FD);

  void handleGSLPointerConstruction(const CXXConstructExpr *CCE);

  /// Detects arguments passed to rvalue reference parameters and creates
  /// MovedOriginFact for them. The MovedLoansAnalysis then uses these facts
  /// to track in a flow-sensitive manner which loans have been moved at each
  /// program point, allowing warnings to distinguish potentially moved storage
  /// from other use-after-free errors.
  void handleMovedArgsInCall(const FunctionDecl *FD,
                             ArrayRef<const Expr *> Args);

  // Handles [[clang::lifetime_capture_by(X)]] annotations on a function call to
  // create flow facts from captured arguments to the capturer
  void handleLifetimeCaptureBy(const FunctionDecl *FD,
                               ArrayRef<const Expr *> Args);

  // Soundness ("safe programming model"): flags call arguments bound to
  // origin-carrying parameters that carry no lifetime annotation and are not
  // modeled via GSL recognition.
  void handleUnannotatedIndirectionArgs(const FunctionDecl *FD,
                                        ArrayRef<const Expr *> Args);

  // Soundness: flags ownership-transferring moves of an owner (std::move of
  // a gsl::Owner, or std::unique_ptr::release), which the analysis does not
  // model.
  void handleMoveSilencing(const Expr *Call, const FunctionDecl *FD,
                           ArrayRef<const Expr *> Args);

  /// Checks if a call-like expression creates a borrow by passing a value to a
  /// reference parameter, creating an IssueFact if it does.
  /// \param IsGslConstruction True if this is a GSL construction where all
  ///   argument origins should flow to the returned origin.
  void handleFunctionCall(const Expr *Call, const FunctionDecl *FD,
                          ArrayRef<const Expr *> Args,
                          bool IsGslConstruction = false);

  // If a call returns a borrow-carrying value (view/pointer/reference) but no
  // loan was propagated into the result, mark it with an Unknown loan so the
  // untracked borrow is reported when used (robustly across dataflow joins).
  void issueUnknownLoanIfUntrackedBorrow(const Expr *Call, OriginNode *CallNode,
                                         bool FlowedIntoResult);


  // Detect methods that invalidate iterators/references/pointees.
  // For instance methods, Args[0] is the implicit 'this' pointer.
  void handleInvalidatingCall(const Expr *Call, const FunctionDecl *FD,
                              ArrayRef<const Expr *> Args);

  // Soundness: conservatively assume that a non-const member call on an
  // owner, or passing an owner to a non-const pointer/reference parameter,
  // invalidates borrows into that owner.
  void handleAssumedInvalidatingCall(const Expr *Call, const FunctionDecl *FD,
                                     ArrayRef<const Expr *> Args);

  // Detect explicit destructor calls/`std::destroy_at`
  void handleDestructiveCall(const Expr *Call, const FunctionDecl *FD,
                             ArrayRef<const Expr *> Args);

  // Detect overlapping (aliasing) call arguments: one argument the call may
  // mutate, and another that borrows it.
  void handleArgumentOverlap(const Expr *Call, const FunctionDecl *FD,
                             ArrayRef<const Expr *> Args);
  void handleAggregateInitOverlap(const Expr *AggExpr,
                                  ArrayRef<const Expr *> Inits);
  void emitArgumentOverlap(const Expr *At, ArrayRef<const Expr *> Args,
                           llvm::function_ref<bool(unsigned)> IsMutatingArg);

  // Detect a callable (lambda or std::function) being invoked -- directly or as
  // a call argument the callee may invoke -- whose value holds borrows of
  // by-reference captures: invoking it may mutate those captured owners,
  // invalidating borrows into them.
  void handleLambdaCallInvalidation(const Expr *Call, const FunctionDecl *FD,
                                    ArrayRef<const Expr *> Args);

  template <typename Destination, typename Source>
  void flowOrigin(const Destination &D, const Source &S) {
    flow(getOriginNode(D), getOriginNode(S), /*Kill=*/false);
  }

  template <typename Destination, typename Source>
  void killAndFlowOrigin(const Destination &D, const Source &S) {
    flow(getOriginNode(D), getOriginNode(S), /*Kill=*/true);
  }

  /// Checks if the expression is a `void("__lifetime_test_point_...")` cast.
  /// If so, creates a `TestPointFact` and returns true.
  bool handleTestPoint(const CXXFunctionalCastExpr *FCE);

  // Treats an expression as a use of the referenced object. It will be
  // checked for use-after-free unless it is later marked as being written to
  // (e.g. on the left-hand side of an assignment in the case of a DeclRefExpr).
  /// Records a use of `E`'s origin. `BoundToReference` says the value is handed to
  /// a callee through a reference or pointer parameter, so the callee keeps
  /// aliasing the designated object: the outer origin -- the one holding the borrow
  /// of the object itself -- is what it receives, and must not be peeled to the
  /// r-value origin the way reading a variable's value is.
  void handleUse(const Expr *E, bool BoundToReference = false);

  /// Soundness: flag a *use* of a global variable whose type is a "container of
  /// indirection" -- an owner/pointer whose elements or pointees are themselves
  /// indirections (e.g. std::vector<std::string_view>, std::span<int*>), or such
  /// a type buried in a non-owner aggregate. The model bans these just like the
  /// local case (VisitDeclStmt); a global's declaration may live outside the
  /// analyzed region (e.g. a header), so the diagnostic is anchored at the use
  /// site. Reported at most once per global per analyzed function. `UseExpr` is
  /// the DeclRefExpr or, for a static data member reached as `obj.member`, the
  /// MemberExpr naming it.
  void handleGlobalContainerOfIndirectionUse(const Expr *UseExpr,
                                             const VarDecl *VD);

  /// Walks the full subtree so origins on the pointee chain and on field
  /// children both escape with the returned value.
  void emitReturnEscapes(OriginNode *N, const Expr *RetExpr);

  bool escapesViaReturn(OriginID OID) const;

  llvm::SmallVector<Fact *> issuePlaceholderLoans();
  FactManager &FactMgr;
  AnalysisDeclContext &AC;
  llvm::SmallVector<Fact *> CurrentBlockFacts;
  // Collect origins that escape the function in this block (OriginEscapesFact),
  // appended at the end of CurrentBlockFacts to ensure they appear after
  // ExpireFact entries.
  llvm::SmallVector<Fact *> EscapesInCurrentBlock;
  // To distinguish between reads and writes for use-after-free checks, this map
  // stores the `UseFact` for each `DeclRefExpr`. We initially identify all
  // `DeclRefExpr`s as "read" uses. When an assignment is processed, the use
  // corresponding to the left-hand side is updated to be a "write", thereby
  // exempting it from the check.
  llvm::DenseMap<const Expr *, UseFact *> UseFacts;
  const CFGBlock *CurrentBlock;
  /// Global "container of indirection" variables already flagged at a use in the
  /// current function (handleGlobalContainerOfIndirectionUse), so a global used
  /// repeatedly is reported once. Cleared per function in run().
  llvm::DenseSet<const VarDecl *> FlaggedIndirectionGlobals;
};

} // namespace clang::lifetimes::internal

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_LIFETIMESAFETY_FACTSGENERATOR_H
