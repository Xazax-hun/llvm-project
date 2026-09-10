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
                                            PathElement Element,
                                            const Expr *ProjectingExpr) {
  const Loan *BaseLoan = getLoan(BaseLoanID);
  // A projection extends a path by one element, and the result is projected again
  // whenever the expression that produced it is re-evaluated. Around a LOOP that
  // never settles: `p = p->next()` with a [[clang::lifetimebound]] accessor turns
  // `n` into `n.*`, then `n.*.*`, ... -- a fresh loan each time, so the memo below
  // never hits and the dataflow has no fixpoint to reach. It did not terminate.
  //
  // Cap the depth and saturate instead. Beyond the cap the element is replaced by
  // an Interior (`.*`) step, and extending a path that already ends in one at the
  // cap yields the same loan -- so projection becomes idempotent and the loan set
  // is finite. `.*` is a may-match wildcard, so it denotes at least what the
  // precise element would have: the collapse can only make paths look like they
  // may alias more, which costs precision and never a missed report.
  const auto &BaseElements = BaseLoan->getAccessPath().getElements();
  // Two consecutive `.*` say nothing more than one. An Interior step absorbs ANY
  // number of elements on either side of a comparison (elementsMayPrefixFrom), so
  // `a.*.*` denotes exactly what `a.*` does: "somewhere inside a". Collapsing them
  // is therefore EXACT, not an approximation -- and it is what makes projection
  // idempotent for the shape that did not terminate, where every step is `.*`.
  if (Element.isInterior() && !BaseElements.empty() &&
      BaseElements.back().isInterior())
    return const_cast<Loan *>(BaseLoan);
  // A MIXED sequence still grows without bound -- `.*`, `.*.next`, `.*.next.*`, ...
  // never repeats, so the collapse above never fires and there is no fixpoint. Cap
  // the depth and saturate into a wildcard: `.*` denotes at least what the precise
  // element would have, so this costs precision and never a missed report.
  if (BaseElements.size() >= MaxProjectionDepth) {
    if (BaseElements.back().isInterior())
      return const_cast<Loan *>(BaseLoan);
    Element = PathElement::getInterior();
  }
  ProjectionCacheKey Key = {BaseLoanID, Element};
  auto [It, Inserted] = LoanProjectionCache.try_emplace(Key, nullptr);
  if (!Inserted)
    return It->second;
  AccessPath ExtendedPath(BaseLoan->getAccessPath(), Element);
  // Keep the base's issuing expression when it has one: it names the storage
  // being borrowed (`Y{}.a` is a borrow of the temporary `Y{}`), which is what
  // a diagnostic wants to point at. Fall back to the access that named the
  // field when the base has no expression at all -- a placeholder base such as
  // `$this` -- which would otherwise leave a borrow of `this->field`
  // unanchored, reportable only from its use.
  //
  // An Interior (`.*`) projection is the exception: it records that the borrow
  // is somewhere inside the base, naming no new storage of its own, so it must
  // not manufacture an anchor the base did not have. Doing so would describe a
  // borrow of a parameter as a borrow of the accessor call, losing the
  // "parameter"/"implicit object" subject the diagnostics report.
  const Expr *Anchor = BaseLoan->getIssuingExpr();
  if (!Anchor && !Element.isInterior())
    Anchor = ProjectingExpr;
  Loan *NewLoan = createLoan(ExtendedPath, Anchor);
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
