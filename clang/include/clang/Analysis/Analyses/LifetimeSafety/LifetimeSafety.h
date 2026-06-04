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

  // Completeness ("safe programming model") diagnostics. These fire wherever the
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

  // Reports a declaration whose type uses more than one level of indirection
  // (e.g. 'int **'), which the analysis cannot fully model.
  virtual void reportMultiLevelIndirection(const ValueDecl *VD) {}

  // Reports an ownership-transferring move of an owner (e.g. 'std::move' of a
  // gsl::Owner, or 'unique_ptr::release'), which the analysis does not model.
  virtual void reportMoveSilencing(const Expr *MoveExpr) {}
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
