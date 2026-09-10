//===- LoanPropagation.cpp - Loan Propagation Analysis ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include <cassert>
#include <memory>

#include "Dataflow.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LoanPropagation.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Loans.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Origins.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Utils.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"

namespace clang::lifetimes::internal {

// Prepass to find persistent origins. An origin is persistent if it is
// referenced in more than one basic block.

namespace {

/// Represents the dataflow lattice for loan propagation.
///
/// This lattice tracks which loans each origin may hold at a given program
/// point.The lattice has a finite height: An origin's loan set is bounded by
/// the total number of loans in the function.
struct Lattice {
  /// The map from an origin to the set of loans it contains.
  /// Origins that appear in multiple blocks. Participates in join operations.
  OriginLoanMap PersistentOrigins = OriginLoanMap(nullptr);
  /// Origins confined to a single block. Discarded at block boundaries.
  OriginLoanMap BlockLocalOrigins = OriginLoanMap(nullptr);

  explicit Lattice(const OriginLoanMap &Persistent,
                   const OriginLoanMap &BlockLocal)
      : PersistentOrigins(Persistent), BlockLocalOrigins(BlockLocal) {}
  Lattice() = default;

  bool operator==(const Lattice &Other) const {
    return PersistentOrigins == Other.PersistentOrigins &&
           BlockLocalOrigins == Other.BlockLocalOrigins;
  }
  bool operator!=(const Lattice &Other) const { return !(*this == Other); }

  void dump(llvm::raw_ostream &OS) const {
    OS << "LoanPropagationLattice State:\n";
    OS << " Persistent Origins:\n";
    if (PersistentOrigins.isEmpty())
      OS << "  <empty>\n";
    for (const auto &Entry : PersistentOrigins) {
      if (Entry.second.isEmpty())
        OS << "  Origin " << Entry.first << " contains no loans\n";
      for (const LoanID &LID : Entry.second)
        OS << "  Origin " << Entry.first << " contains Loan " << LID << "\n";
    }
    OS << " Block-Local Origins:\n";
    if (BlockLocalOrigins.isEmpty())
      OS << "  <empty>\n";
    for (const auto &Entry : BlockLocalOrigins) {
      if (Entry.second.isEmpty())
        OS << "  Origin " << Entry.first << " contains no loans\n";
      for (const LoanID &LID : Entry.second)
        OS << "  Origin " << Entry.first << " contains Loan " << LID << "\n";
    }
  }
};

class AnalysisImpl
    : public DataflowAnalysis<AnalysisImpl, Lattice, Direction::Forward> {
public:
  AnalysisImpl(const CFG &C, AnalysisDeclContext &AC, FactManager &F,
               OriginLoanMap::Factory &OriginLoanMapFactory,
               LoanSet::Factory &LoanSetFactory)
      : DataflowAnalysis(C, AC, F), OriginLoanMapFactory(OriginLoanMapFactory),
        LoanSetFactory(LoanSetFactory),
        PersistentOrigins(F.getPersistentOrigins(C)) {}

  using Base::transfer;

  StringRef getAnalysisName() const { return "LoanPropagation"; }

  Lattice getInitialState() { return Lattice{}; }

  /// Merges two lattices by taking the union of loans for each origin.
  Lattice join(Lattice A, Lattice B) {
    assert(A.BlockLocalOrigins.isEmpty() && B.BlockLocalOrigins.isEmpty() &&
           "block-local origins must not reach a block boundary");
    OriginLoanMap JoinedOrigins = utils::join(
        A.PersistentOrigins, B.PersistentOrigins, OriginLoanMapFactory,
        [&](const LoanSet *S1, const LoanSet *S2) {
          assert((S1 || S2) && "unexpectedly merging 2 empty sets");
          if (!S1)
            return *S2;
          if (!S2)
            return *S1;
          return utils::join(*S1, *S2, LoanSetFactory);
        },
        // Asymmetric join is a performance win. For origins present only on one
        // branch, the loan set can be carried over as-is.
        utils::JoinKind::Asymmetric);
    return Lattice(JoinedOrigins, OriginLoanMapFactory.getEmptyMap());
  }

  /// Block-local origins are not referenced outside the block that computed
  /// them, so they are dropped here rather than propagated to adjacent blocks.
  /// Dropping them at the boundary (instead of in `join`) also covers edges
  /// where `join` is never called, such as blocks with a single predecessor.
  Lattice transferAtBlockExit(Lattice L) {
    return Lattice(L.PersistentOrigins, OriginLoanMapFactory.getEmptyMap());
  }

  /// A new loan is issued to the origin. Old loans are erased.
  Lattice transfer(Lattice In, const IssueFact &F) {
    OriginID OID = F.getOriginID();
    LoanID LID = F.getLoanID();
    LoanSet NewLoans = LoanSetFactory.add(LoanSetFactory.getEmptySet(), LID);
    return setLoans(In, OID, NewLoans);
  }

  /// A flow from source to destination. If `KillDest` is true, this replaces
  /// the destination's loans with the source's. Otherwise, the source's loans
  /// are merged into the destination's.
  Lattice transfer(Lattice In, const OriginFlowFact &F) {
    OriginID DestOID = F.getDestOriginID();
    OriginID SrcOID = F.getSrcOriginID();

    LoanSet DestLoans =
        F.getKillDest() ? LoanSetFactory.getEmptySet() : getLoans(In, DestOID);
    LoanSet SrcLoans = getLoans(In, SrcOID);
    LoanSet MergedLoans = utils::join(DestLoans, SrcLoans, LoanSetFactory);

    return setLoans(In, DestOID, MergedLoans);
  }

  /// A projection extends, in place, each loan the origin holds by one path
  /// element: `{x}` projected through `.f` becomes `{x.f}`. The projected loans
  /// are memoized in the LoanManager so repeated evaluation converges.
  Lattice transfer(Lattice In, const ProjectionFact &F) {
    OriginID OID = F.getOriginID();
    LoanSet Loans = getLoans(In, OID);
    LoanSet ProjectedLoans = LoanSetFactory.getEmptySet();
    PathElement Element = F.getPathElement();
    for (LoanID LID : Loans) {
      Loan *Projected = FactMgr.getLoanMgr().getOrCreateProjectedLoan(
          LID, Element, F.getProjectingExpr());
      ProjectedLoans = LoanSetFactory.add(ProjectedLoans, Projected->getID());
    }
    return setLoans(In, OID, ProjectedLoans);
  }

  /// A store whose destinations are named by the loans its lvalue holds.
  ///
  /// Routing happens here, not in the fact generator, because the destinations
  /// ARE those loans and they only exist once propagation has run. Every
  /// destination is MERGED into: with more than one candidate the analysis
  /// cannot tell which was written, so a destination's earlier loans must
  /// survive.
  ///
  /// A loan names the storage written, and getOriginForAccessPath turns that
  /// name into the origin holding it -- including a subobject, which resolves
  /// to the very origin a later read of the same member consults. A loan whose
  /// ROOT does not resolve (an unknown borrow) designates storage this transfer
  /// cannot reach; those are left alone here and reported by the checker, so
  /// the store is refused rather than silently dropped.
  Lattice transfer(Lattice In, const DynamicStoreFact &F) {
    // Whether anything the store had to reach was missed is decided HERE, where
    // the pre-store state is in hand. Asking afterwards is wrong when the
    // destination lvalue is itself a destination -- `this`, whose lvalue origin
    // IS the object's -- because by then it also holds what this very store
    // deposited, and judging the payload as a destination refuses a store that
    // landed perfectly. Recomputing it in the checker also duplicated these
    // rules; now there is one copy. Overwritten on each visit, so the converged
    // iteration's verdict is the one that stands.
    bool Unresolved = false;
    LoanSet DestLoans = getLoans(In, F.getDestLValueOrigin());
    Lattice Out = In;
    LoanSet SrcLoans = getLoans(In, F.getSrcOrigin());
    if (DestLoans.isEmpty())
      Unresolved = true;
    for (LoanID LID : DestLoans) {
      const AccessPath &AP = FactMgr.getLoanMgr().getLoan(LID)->getAccessPath();
      // A loan names the storage the store lands in; that storage has an origin
      // whenever the path resolves -- including a subobject, whose origin is
      // the one a later read of the same member consults.
      if (const OriginNode *Dest =
              FactMgr.getOriginMgr().getOriginForAccessPath(AP)) {
        if (!SrcLoans.isEmpty()) {
          OriginID DestOID = Dest->getOriginID();
          Out = setLoans(
              Out, DestOID,
              utils::join(getLoans(Out, DestOID), SrcLoans, LoanSetFactory));
        }
        continue;
      }
      // Fallback for a temporary with no origin node of its own (its type is not
      // tracked): one that is not lifetime-extended dies at the end of the full
      // expression, so nothing can read a borrow stored into it afterwards -- no
      // loss to report, and refusing would fire on every `Widget{}.set(x)`. A
      // temporary WITH origins resolves above and is tracked instead; that is not
      // merely a precision gain, because a non-extended temporary's own DESTRUCTOR
      // runs at the cleanup and can read the borrow, which this exemption misses.
      if (const auto *MTE = AP.getAsMaterializeTemporaryExpr();
          MTE && !MTE->getExtendingDecl())
        continue;
      Unresolved = true;
    }
    UnresolvedStores[&F] = Unresolved;
    // The loans the lvalue held BEFORE the store. Anyone asking afterwards sees
    // the store's own payload mixed in (when the lvalue is itself a
    // destination, as `this` is), which would make the payload look like the
    // destination.
    auto &Recorded = PreStoreDestLoans[&F];
    Recorded.clear();
    for (LoanID LID : DestLoans)
      Recorded.push_back(LID);
    return Out;
  }

  llvm::ArrayRef<LoanID>
  getPreStoreDestinationLoans(const DynamicStoreFact *DSF) const {
    auto It = PreStoreDestLoans.find(DSF);
    return It != PreStoreDestLoans.end() ? llvm::ArrayRef<LoanID>(It->second)
                                         : llvm::ArrayRef<LoanID>();
  }

  bool hasUnresolvedStoreDestination(const DynamicStoreFact *DSF) const {
    auto It = UnresolvedStores.find(DSF);
    return It != UnresolvedStores.end() && It->second;
  }

  Lattice transfer(Lattice In, const KillOriginFact &F) {
    return setLoans(In, F.getKilledOrigin(), LoanSetFactory.getEmptySet());
  }

  Lattice transfer(Lattice In, const ExpireFact &F) {
    if (auto OID = F.getOriginID())
      return setLoans(In, *OID, LoanSetFactory.getEmptySet());
    return In;
  }

  LoanSet getLoans(OriginID OID, ProgramPoint P) const {
    return getLoans(getState(P), OID);
  }

  llvm::SmallVector<OriginID>
  buildOriginFlowChain(ProgramPoint StartPoint, const OriginID StartOID,
                       const LoanID TargetLoan) const {
    // Precondition: the caller asks about a loan that really is held by this
    // origin here. (Upstream keeps this assertion too.)
    assert(getLoans(StartOID, StartPoint).contains(TargetLoan) &&
           "TargetLoan must be present in the StartOID at the StartPoint");

    // The loan's IDENTITY changes as it flows: a ProjectionFact replaces a loan
    // with a projection of it, so the loan issued upstream is not the loan
    // reaching StartPoint. Track the current loan alongside the current origin
    // and step back across projections (below), or the walk loses the borrow at
    // the first projection and never reaches its IssueFact.
    OriginID CurrOID = StartOID;
    LoanID CurrLoanID = TargetLoan;
    llvm::SmallVector<OriginID> OriginFlowChain;
    llvm::ArrayRef<const Fact *> Facts = FactMgr.getBlockContaining(StartPoint);
    const auto *StartIt = llvm::find(Facts, StartPoint);
    assert(StartIt != Facts.end());

    auto StateBefore = [&](const Fact *F) {
      const auto *It = llvm::find(Facts, F);
      assert(It != Facts.end());
      // This walk is confined to one block (see the FIXME below), so the state
      // before its first fact is not reachable from here; treat it as empty,
      // which just declines the step-back rather than taking a wrong one.
      return It == Facts.begin() ? Lattice{} : getState(*(It - 1));
    };

    for (const Fact *F :
         llvm::reverse(llvm::make_range(Facts.begin(), StartIt))) {
      if (const auto *IF = F->getAs<IssueFact>()) {
        if (IF->getLoanID() == CurrLoanID && IF->getOriginID() == CurrOID)
          return OriginFlowChain;
        continue;
      }
      if (const auto *PF = F->getAs<ProjectionFact>()) {
        // Step back from a projected loan to the loan it was projected from
        // (`obj.field` -> `obj`), so the walk keeps following one borrow even
        // though its identity changed here.
        if (PF->getOriginID() != CurrOID)
          continue;
        if (std::optional<LoanID> BaseLoanID =
                FactMgr.getLoanMgr().getBaseLoan(CurrLoanID))
          if (getLoans(StateBefore(PF), CurrOID).contains(*BaseLoanID))
            CurrLoanID = *BaseLoanID;
        continue;
      }
      const auto *OFF = F->getAs<OriginFlowFact>();
      if (!OFF)
        continue;
      if (OFF->getDestOriginID() != CurrOID)
        continue;

      const OriginID SrcOriginID = OFF->getSrcOriginID();
      if (!getLoans(SrcOriginID, OFF).contains(CurrLoanID))
        continue;
      OriginFlowChain.push_back(SrcOriginID);
      CurrOID = SrcOriginID;
    }

    // FIXME: Ideally, this return is unreachable and should be an assert
    // because we expect to always finish at an IssueFact. But since current
    // traversal is limited to a single CFG block, multi-block OriginFlowChain
    // construction might miss the IssueFact. We should add llvm_unreachable
    // here once multi-block support is implemented.
    return {};
  }

private:
  /// Returns true if the origin is persistent (referenced in multiple blocks).
  bool isPersistent(OriginID OID) const {
    return PersistentOrigins.test(OID.Value);
  }

  Lattice setLoans(Lattice L, OriginID OID, LoanSet Loans) {
    if (isPersistent(OID))
      return Lattice(OriginLoanMapFactory.add(L.PersistentOrigins, OID, Loans),
                     L.BlockLocalOrigins);
    return Lattice(L.PersistentOrigins,
                   OriginLoanMapFactory.add(L.BlockLocalOrigins, OID, Loans));
  }

  LoanSet getLoans(Lattice L, OriginID OID) const {
    const OriginLoanMap *Map =
        isPersistent(OID) ? &L.PersistentOrigins : &L.BlockLocalOrigins;
    if (auto *Loans = Map->lookup(OID))
      return *Loans;
    return LoanSetFactory.getEmptySet();
  }

  OriginLoanMap::Factory &OriginLoanMapFactory;
  LoanSet::Factory &LoanSetFactory;
  /// Per dynamic store, whether anything it had to reach was missed. Decided in
  /// the transfer, where the PRE-store state is in hand.
  llvm::DenseMap<const DynamicStoreFact *, bool> UnresolvedStores;
  /// Per dynamic store, the loans its destination lvalue held before it ran.
  llvm::DenseMap<const DynamicStoreFact *, llvm::SmallVector<LoanID, 4>>
      PreStoreDestLoans;
  /// Boolean vector indexed by origin ID. If true, the origin appears in
  /// multiple basic blocks and must participate in join operations. If false,
  /// the origin is block-local and can be discarded at block boundaries.
  /// Shared with every other analysis via FactManager; see
  /// getPersistentOrigins.
  const llvm::BitVector &PersistentOrigins;
};
} // namespace

class LoanPropagationAnalysis::Impl final : public AnalysisImpl {
  using AnalysisImpl::AnalysisImpl;
};

LoanPropagationAnalysis::LoanPropagationAnalysis(
    const CFG &C, AnalysisDeclContext &AC, FactManager &F,
    OriginLoanMap::Factory &OriginLoanMapFactory,
    LoanSet::Factory &LoanSetFactory)
    : PImpl(std::make_unique<Impl>(C, AC, F, OriginLoanMapFactory,
                                   LoanSetFactory)) {
  PImpl->run();
}

LoanPropagationAnalysis::~LoanPropagationAnalysis() = default;

LoanSet LoanPropagationAnalysis::getLoans(OriginID OID, ProgramPoint P) const {
  return PImpl->getLoans(OID, P);
}

llvm::ArrayRef<LoanID> LoanPropagationAnalysis::getPreStoreDestinationLoans(
    const DynamicStoreFact *DSF) const {
  return PImpl->getPreStoreDestinationLoans(DSF);
}

bool LoanPropagationAnalysis::hasUnresolvedStoreDestination(
    const DynamicStoreFact *DSF) const {
  return PImpl->hasUnresolvedStoreDestination(DSF);
}

llvm::SmallVector<OriginID>
LoanPropagationAnalysis::buildOriginFlowChain(ProgramPoint StartPoint,
                                              const OriginID StartOID,
                                              const LoanID TargetLoan) const {
  return PImpl->buildOriginFlowChain(StartPoint, StartOID, TargetLoan);
}
} // namespace clang::lifetimes::internal
