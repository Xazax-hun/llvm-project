//===- Loans.cpp - Loan Implementation --------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/Analyses/LifetimeSafety/Loans.h"

namespace clang::lifetimes::internal {

void AccessPath::dump(llvm::raw_ostream &OS) const {
  switch (K) {
  case Kind::ValueDecl:
    if (const clang::ValueDecl *VD = getAsValueDecl())
      OS << VD->getNameAsString();
    break;
  case Kind::MaterializeTemporary:
    if (const clang::MaterializeTemporaryExpr *MTE =
            getAsMaterializeTemporaryExpr())
      OS << "MaterializeTemporaryExpr at " << MTE;
    break;
  case Kind::PlaceholderParam:
    if (const auto *PVD = getAsPlaceholderParam())
      OS << "$" << PVD->getNameAsString();
    break;
  case Kind::PlaceholderThis:
    OS << "$this";
    break;
  case Kind::NewAllocation:
    if (const auto *E = getAsNewAllocation())
      OS << "NewAllocation at " << E;
    break;
  case Kind::Immortal:
    OS << "Immortal";
    if (const auto *FD = getAsImmortal())
      OS << " " << FD->getNameAsString();
    break;
  case Kind::Uninitialized:
    OS << "Uninitialized";
    if (const auto *D = dyn_cast_if_present<clang::ValueDecl>(
            Root.dyn_cast<const clang::Decl *>()))
      OS << " " << D->getNameAsString();
    break;
  case Kind::Unknown:
    OS << "Unknown";
    break;
  }
  for (const PathElement &E : Elements)
    E.dump(OS);
}

void Loan::dump(llvm::raw_ostream &OS) const {
  OS << getID() << " (Path: ";
  Path.dump(OS);
  OS << ")";
}
Loan *LoanManager::getOrCreateProjectedLoan(LoanID BaseLoanID,
                                            PathElement Element) {
  ProjectionCacheKey Key = {BaseLoanID, Element};
  auto [It, Inserted] = LoanProjectionCache.try_emplace(Key, nullptr);
  if (!Inserted)
    return It->second;
  const Loan *BaseLoan = getLoan(BaseLoanID);
  AccessPath ExtendedPath(BaseLoan->getAccessPath(), Element);
  Loan *NewLoan = createLoan(ExtendedPath, BaseLoan->getIssuingExpr());
  BaseLoansMap[NewLoan->getID()] = BaseLoanID;
  // try_emplace may have rehashed during createLoan; re-find rather than reuse
  // the iterator.
  LoanProjectionCache[Key] = NewLoan;
  return NewLoan;
}

std::optional<LoanID> LoanManager::getBaseLoan(LoanID ProjectedLoanID) const {
  auto It = BaseLoansMap.find(ProjectedLoanID);
  if (It != BaseLoansMap.end())
    return It->second;
  return std::nullopt;
}
} // namespace clang::lifetimes::internal
