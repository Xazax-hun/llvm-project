//===- Facts.cpp - Lifetime Analysis Facts Implementation -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/AST/Decl.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "llvm/Support/TimeProfiler.h"

namespace clang::lifetimes::internal {

/// The origins a dynamic store can WRITE, over-approximated statically.
///
/// A dynamic store's destinations are the loans its lvalue holds, which the
/// prepass cannot evaluate -- but it can bound them. Every loan in that lvalue
/// was ISSUED into some origin that flows into it, so walking the flow edges
/// backwards from the lvalue's origin and collecting the loans issued anywhere
/// in that reachable set gives a superset of the loans it can hold. Each such
/// loan names the storage the store can land in, and that storage has an
/// origin.
///
/// Marking only these keeps the block-local fast path for everything else. The
/// cheaper approximations are both far too coarse in practice: marking every
/// origin, or every declaration's origin, cost ~14x on this pass for a single
/// lifetime_capture_by in a 60-variable, 120-block function, because in such a
/// function the declaration origins are exactly the ones carrying loans.
static void collectDynamicStoreDestinations(const FactManager &FactMgr,
                                            const CFG &C,
                                            llvm::BitVector &Out) {
  llvm::SmallVector<const DynamicStoreFact *> Stores;
  // Reverse flow edges: for each origin, the origins that flow into it.
  llvm::DenseMap<unsigned, llvm::SmallVector<OriginID, 2>> FlowsInto;
  // Loans issued to each origin.
  llvm::DenseMap<unsigned, llvm::SmallVector<LoanID, 2>> IssuedTo;
  for (const CFGBlock *B : C)
    for (const Fact *F : FactMgr.getFacts(B)) {
      if (const auto *DS = F->getAs<DynamicStoreFact>())
        Stores.push_back(DS);
      else if (const auto *OF = F->getAs<OriginFlowFact>())
        FlowsInto[OF->getDestOriginID().Value].push_back(OF->getSrcOriginID());
      else if (const auto *IF = F->getAs<IssueFact>())
        IssuedTo[IF->getOriginID().Value].push_back(IF->getLoanID());
    }
  if (Stores.empty())
    return;

  const OriginManager &OM = FactMgr.getOriginMgr();
  llvm::BitVector Seen(OM.getNumOrigins());
  llvm::SmallVector<OriginID> Work;
  for (const DynamicStoreFact *DS : Stores) {
    OriginID Start = DS->getDestLValueOrigin();
    if (!Seen.test(Start.Value)) {
      Seen.set(Start.Value);
      Work.push_back(Start);
    }
  }
  while (!Work.empty()) {
    OriginID Cur = Work.pop_back_val();
    // Any loan issued into a reachable origin may reach the store's lvalue.
    for (LoanID LID : IssuedTo.lookup(Cur.Value)) {
      const AccessPath &AP = FactMgr.getLoanMgr().getLoan(LID)->getAccessPath();
      // Exactly the origin the routing would write, so the two stay in step:
      // a destination the routing can reach but the prepass does not mark is
      // one whose deposit is discarded at the next block boundary.
      if (const OriginNode *Dest = OM.getOriginForAccessPath(AP))
        Out.set(Dest->getOriginID().Value);
    }
    for (OriginID Pred : FlowsInto.lookup(Cur.Value))
      if (!Seen.test(Pred.Value)) {
        Seen.set(Pred.Value);
        Work.push_back(Pred);
      }
  }
}

void FactManager::computePersistentOrigins(const CFG &C) {
  llvm::TimeTraceScope TimeProfile("ComputePersistentOrigins");
  const FactManager &FactMgr = *this;
  unsigned NumOrigins = FactMgr.getOriginMgr().getNumOrigins();
  PersistentOrigins.resize(NumOrigins);

  llvm::SmallVector<const CFGBlock *> OriginToFirstSeenBlock(NumOrigins,
                                                             nullptr);
  for (const CFGBlock *B : C) {
    for (const Fact *F : FactMgr.getFacts(B)) {
      auto CheckOrigin = [&](OriginID OID) {
        if (PersistentOrigins.test(OID.Value))
          return;
        auto &FirstSeenBlock = OriginToFirstSeenBlock[OID.Value];
        if (FirstSeenBlock == nullptr)
          FirstSeenBlock = B;
        if (FirstSeenBlock != B) {
          // We saw this origin in more than one block.
          PersistentOrigins.set(OID.Value);
        }
      };

      switch (F->getKind()) {
      case Fact::Kind::Issue:
        CheckOrigin(F->getAs<IssueFact>()->getOriginID());
        break;
      case Fact::Kind::OriginFlow: {
        const auto *OF = F->getAs<OriginFlowFact>();
        CheckOrigin(OF->getDestOriginID());
        CheckOrigin(OF->getSrcOriginID());
        break;
      }
      case Fact::Kind::Use: {
        // Walk the WHOLE origin subtree, field edges included -- not just the
        // pointee chain. LiveOrigins' transfer for this fact marks every node
        // under `children()` live, so a node it can mark has to be registered
        // here too: liveness now travels in the persistent half only, and a
        // field child named nowhere else in this block would be classified
        // block-local, have its liveness dropped at the block boundary, and
        // stop reaching an expiry in the block that issued its loan.
        llvm::SmallVector<const OriginNode *> Work{
            F->getAs<UseFact>()->getUsedOrigins()};
        while (!Work.empty()) {
          const OriginNode *Cur = Work.pop_back_val();
          if (!Cur)
            continue;
          CheckOrigin(Cur->getOriginID());
          for (const OriginNode::Edge &E : Cur->children())
            Work.push_back(E.Child);
        }
        break;
      }
      case Fact::Kind::KillOrigin:
        CheckOrigin(F->getAs<KillOriginFact>()->getKilledOrigin());
        break;
      case Fact::Kind::OriginEscapes:
        // An origin that escapes (via return/field/global) is defined in some
        // earlier block and read here at the escape point; it spans blocks and
        // must participate in joins. Omitting it misclassifies an origin that
        // is only conditionally assigned and escapes at the exit block as
        // block-local, dropping its loans at the join before the escape/expiry
        // check (e.g. a conditional store of a stack address to a global).
        CheckOrigin(F->getAs<OriginEscapesFact>()->getEscapedOriginID());
        break;
      case Fact::Kind::DynamicStore: {
        // The origins this fact READS are ordinary reads and must be registered
        // like any other, or a store whose block mentions them nowhere else
        // leaves them block-local -- their loans are then dropped at the
        // boundary, the destination lvalue looks like it holds nothing, and the
        // store is refused as unresolvable even though it names a perfectly
        // good object.
        const auto *DS = F->getAs<DynamicStoreFact>();
        CheckOrigin(DS->getDestLValueOrigin());
        CheckOrigin(DS->getSrcOrigin());
        // Where the store LANDS is a different question -- those origins are
        // not named by this fact -- and is bounded once for the whole function
        // by collectDynamicStoreDestinations, which needs the flow graph rather
        // than one fact at a time.
        break;
      }
      // Every fact below READS an origin that some earlier block may have
      // written, so each has to register it -- the same reason spelled out for
      // OriginEscapes and DynamicStore above. Leaving one out makes an origin
      // mentioned nowhere else in its block look block-local, and its loans are
      // dropped at the boundary: the fact then sees an empty origin and decides
      // there is nothing to say.
      //
      // InvalidateOrigin is how this was found. A structured binding expands
      // every use to the SAME MemberExpr, so one origin carries the member
      // across the whole function; with the mutation inside a loop, the
      // invalidation names an origin projected in an earlier block, saw no
      // loans, and reported nothing -- while the identical loop written
      // `rec.samples` re-projects in the loop body and was reported.
      case Fact::Kind::InvalidateOrigin:
        CheckOrigin(F->getAs<InvalidateOriginFact>()->getInvalidatedOrigin());
        break;
      case Fact::Kind::Projection:
        CheckOrigin(F->getAs<ProjectionFact>()->getOriginID());
        break;
      case Fact::Kind::FieldStore: {
        const auto *FS = F->getAs<FieldStoreFact>();
        CheckOrigin(FS->getStoredOrigin());
        CheckOrigin(FS->getContainerOrigin());
        break;
      }
      case Fact::Kind::ArgumentOverlap: {
        const auto *AO = F->getAs<ArgOverlapFact>();
        for (OriginID OID : AO->getMutatingOrigins())
          CheckOrigin(OID);
        for (OriginID OID : AO->getBorrowOrigins())
          CheckOrigin(OID);
        break;
      }
      // These name no origin.
      case Fact::Kind::MovedOrigin:
      case Fact::Kind::Expire:
      case Fact::Kind::TestPoint:
      case Fact::Kind::UntrackedConstruct:
        break;
      }
    }
  }
  // A dynamic store writes origins this per-fact walk cannot name, so its
  // destinations are bounded separately and marked unconditionally: the store
  // may be in a different block from every other mention of the destination.
  collectDynamicStoreDestinations(FactMgr, C, PersistentOrigins);
}

void Fact::dump(llvm::raw_ostream &OS, const LoanManager &,
                const OriginManager &) const {
  OS << "Fact (Kind: " << static_cast<int>(K) << ")\n";
}

void IssueFact::dump(llvm::raw_ostream &OS, const LoanManager &LM,
                     const OriginManager &OM) const {
  OS << "Issue (";
  LM.getLoan(getLoanID())->dump(OS);
  OS << ", ToOrigin: ";
  OM.dump(getOriginID(), OS);
  OS << ")\n";
}

void ExpireFact::dump(llvm::raw_ostream &OS, const LoanManager &LM,
                      const OriginManager &OM) const {
  OS << "Expire (";
  getAccessPath().dump(OS);
  if (auto OID = getOriginID()) {
    OS << ", Origin: ";
    OM.dump(*OID, OS);
  }
  OS << ")\n";
}

void OriginFlowFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                          const OriginManager &OM) const {
  OS << "OriginFlow: \n";
  OS << "\tDest: ";
  OM.dump(getDestOriginID(), OS);
  OS << "\n";
  OS << "\tSrc:  ";
  OM.dump(getSrcOriginID(), OS);
  OS << (getKillDest() ? "" : ", Merge");
  OS << "\n";
}

void MovedOriginFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                           const OriginManager &OM) const {
  OS << "MovedOrigins (";
  OM.dump(getMovedOrigin(), OS);
  OS << ")\n";
}

void ReturnEscapeFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                            const OriginManager &OM) const {
  OS << "OriginEscapes (";
  OM.dump(getEscapedOriginID(), OS);
  OS << ", via Return)\n";
}

void FieldEscapeFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                           const OriginManager &OM) const {
  OS << "OriginEscapes (";
  OM.dump(getEscapedOriginID(), OS);
  OS << ", via Field)\n";
}

void GlobalEscapeFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                            const OriginManager &OM) const {
  OS << "OriginEscapes (";
  OM.dump(getEscapedOriginID(), OS);
  OS << ", via Global)\n";
}

void CapturedByThisEscapeFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                                    const OriginManager &OM) const {
  OS << "OriginEscapes (";
  OM.dump(getEscapedOriginID(), OS);
  OS << ", via CapturedByThis)\n";
}

void ObjectEscapeFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                            const OriginManager &OM) const {
  OS << "OriginEscapes (";
  OM.dump(getEscapedOriginID(), OS);
  OS << ", via Object)\n";
}

// Recursively prints every origin in the subtree rooted at `N`.
static void dumpUsedOrigins(const OriginNode *N, const FieldDecl *FD,
                            const OriginManager &OM, llvm::raw_ostream &OS,
                            bool &First) {
  if (!N)
    return;
  if (!First)
    OS << ", ";
  First = false;
  OM.dump(N->getOriginID(), OS, FD);
  for (const OriginNode::Edge &E : N->children())
    dumpUsedOrigins(E.Child, E.FD, OM, OS, First);
}

void ProjectionFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                          const OriginManager &OM) const {
  OS << "Projection (";
  OM.dump(getOriginID(), OS);
  OS << ", Element: ";
  getPathElement().dump(OS);
  OS << ")\n";
}

void UseFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                   const OriginManager &OM) const {
  OS << "Use (";
  bool First = true;
  dumpUsedOrigins(getUsedOrigins(), nullptr, OM, OS, First);
  OS << ", " << (isWritten() ? "Write" : "Read") << ")\n";
}

void InvalidateOriginFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                                const OriginManager &OM) const {
  OS << "InvalidateOrigin (";
  OM.dump(getInvalidatedOrigin(), OS);
  OS << ")\n";
}

void TestPointFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                         const OriginManager &) const {
  OS << "TestPoint (Annotation: \"" << getAnnotation() << "\")\n";
}

void KillOriginFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                          const OriginManager &OM) const {
  OS << "KillOrigin (";
  OM.dump(getKilledOrigin(), OS);
  OS << ")\n";
}

void UntrackedConstructFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                                  const OriginManager &) const {
  OS << "UntrackedConstruct (";
  switch (getReason()) {
  case UntrackedConstructReason::IndirectCall:
    OS << "IndirectCall";
    break;
  case UntrackedConstructReason::UnannotatedIndirection:
    OS << "UnannotatedIndirection";
    break;
  case UntrackedConstructReason::MoveSilencing:
    OS << "MoveSilencing";
    break;
  case UntrackedConstructReason::UnknownOwnership:
    OS << "UnknownOwnership";
    break;
  case UntrackedConstructReason::Exception:
    OS << "Exception";
    break;
  case UntrackedConstructReason::OwnerOfIndirection:
    OS << "OwnerOfIndirection";
    break;
  case UntrackedConstructReason::PointerOfIndirection:
    OS << "PointerOfIndirection";
    break;
  case UntrackedConstructReason::ViewOnMutableGlobal:
    OS << "ViewOnMutableGlobal";
    break;
  case UntrackedConstructReason::ConstMethodIndirectMutation:
    OS << "ConstMethodIndirectMutation";
    break;
  case UntrackedConstructReason::MultiLevelIndirectionExpr:
    OS << "MultiLevelIndirectionExpr";
    break;
  case UntrackedConstructReason::Union:
    OS << "Union";
    break;
  case UntrackedConstructReason::ReinterpretCast:
    OS << "ReinterpretCast";
    break;
  case UntrackedConstructReason::VoidPointerCast:
    OS << "VoidPointerCast";
    break;
  case UntrackedConstructReason::Downcast:
    OS << "Downcast";
    break;
  case UntrackedConstructReason::LambdaRefCaptureIndirection:
    OS << "LambdaRefCaptureIndirection";
    break;
  case UntrackedConstructReason::CaptureIntoBorrowlessObject:
    OS << "CaptureIntoBorrowlessObject";
    break;
  case UntrackedConstructReason::BinaryConditionalTemporary:
    OS << "BinaryConditionalTemporary";
    break;
  case UntrackedConstructReason::ArrayOfIndirectionDecay:
    OS << "ArrayOfIndirectionDecay";
    break;
  case UntrackedConstructReason::UnsupportedStoreDestination:
    OS << "UnsupportedStoreDestination";
    break;
  case UntrackedConstructReason::SetjmpLongjmp:
    OS << "SetjmpLongjmp";
    break;
  case UntrackedConstructReason::Coroutine:
    OS << "Coroutine";
    break;
  }
  OS << ")\n";
}

void DynamicStoreFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                            const OriginManager &OM) const {
  OS << "DynamicStore (Into loans of: ";
  OM.dump(getDestLValueOrigin(), OS);
  OS << ", Stored: ";
  OM.dump(getSrcOrigin(), OS);
  OS << ")\n";
}

void FieldStoreFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                          const OriginManager &OM) const {
  OS << "FieldStore (Stored: ";
  OM.dump(getStoredOrigin(), OS);
  OS << ", Container: ";
  OM.dump(getContainerOrigin(), OS);
  OS << ")\n";
}

void ArgOverlapFact::dump(llvm::raw_ostream &OS, const LoanManager &,
                          const OriginManager &OM) const {
  OS << "ArgOverlap (Mutating: [";
  bool FirstMut = true;
  for (OriginID M : getMutatingOrigins()) {
    if (!FirstMut)
      OS << ", ";
    FirstMut = false;
    OM.dump(M, OS);
  }
  OS << "], Borrows: [";
  bool First = true;
  for (OriginID B : getBorrowOrigins()) {
    if (!First)
      OS << ", ";
    First = false;
    OM.dump(B, OS);
  }
  OS << "])\n";
}

llvm::StringMap<ProgramPoint> FactManager::getTestPoints() const {
  llvm::StringMap<ProgramPoint> AnnotationToPointMap;
  for (const auto &BlockFacts : BlockToFacts) {
    for (const Fact *F : BlockFacts) {
      if (const auto *TPF = F->getAs<TestPointFact>()) {
        StringRef PointName = TPF->getAnnotation();
        assert(!AnnotationToPointMap.contains(PointName) &&
               "more than one test points with the same name");
        AnnotationToPointMap[PointName] = F;
      }
    }
  }
  return AnnotationToPointMap;
}

void FactManager::dump(const CFG &Cfg, AnalysisDeclContext &AC) const {
  llvm::dbgs() << "==========================================\n";
  llvm::dbgs() << "       Lifetime Analysis Facts:\n";
  llvm::dbgs() << "==========================================\n";
  if (const Decl *D = AC.getDecl())
    if (const auto *ND = dyn_cast<NamedDecl>(D))
      llvm::dbgs() << "Function: " << ND->getQualifiedNameAsString() << "\n";
  // Print blocks in the order as they appear in code for a stable ordering.
  for (const CFGBlock *B : *AC.getAnalysis<PostOrderCFGView>()) {
    llvm::dbgs() << "  Block B" << B->getBlockID() << ":\n";
    for (const Fact *F : getFacts(B)) {
      llvm::dbgs() << "    ";
      F->dump(llvm::dbgs(), LoanMgr, OriginMgr);
    }
    llvm::dbgs() << "  End of Block\n";
  }
}

llvm::ArrayRef<const Fact *>
FactManager::getBlockContaining(ProgramPoint P) const {
  for (const auto &BlockToFactsVec : BlockToFacts) {
    for (const Fact *F : BlockToFactsVec)
      if (F == P)
        return BlockToFactsVec;
  }
  return {};
}

} // namespace clang::lifetimes::internal
