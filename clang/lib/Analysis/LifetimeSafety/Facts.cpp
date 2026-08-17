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

namespace clang::lifetimes::internal {

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
  case UntrackedConstructReason::Downcast:
    OS << "Downcast";
    break;
  case UntrackedConstructReason::LambdaRefCaptureIndirection:
    OS << "LambdaRefCaptureIndirection";
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
