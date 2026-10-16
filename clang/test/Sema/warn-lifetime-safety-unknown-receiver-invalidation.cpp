// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -std=c++23 -I%t -I%S/Inputs \
// RUN:   -Wlifetime-safety-soundness -verify %t/use.cpp

//--- sysmax.h
#pragma clang system_header
#include "lifetime-analysis.h"
// The signature of std::max/std::min/std::clamp: a reference to a pointer in and
// out. That is two levels of indirection, which the model cannot represent -- but
// the refusal covering it (-Wlifetime-safety-multilevel-indirection) is a
// DECLARATION-site diagnostic, and declaration-site refusals are suppressed in
// system headers. Declare this signature yourself and you get three refusals;
// behind `#pragma clang system_header` the translation unit was silent. The
// lifetimebound annotations mean -Wlifetime-safety-unannotated-indirection does
// not fire either. Since the whole standard library is a system header, this shape
// was a ready-made loan sink.
std::vector<int> *const &sysmax(std::vector<int> *const &a [[clang::lifetimebound]],
                                std::vector<int> *const &b [[clang::lifetimebound]]);

struct W {
  std::vector<int> v;
  void grow();
  const int *first() const [[clang::lifetimebound]];
};
W *const &sysmaxW(W *const &a [[clang::lifetimebound]],
                  W *const &b [[clang::lifetimebound]]);

//--- use.cpp
#include "sysmax.h"

volatile int sink;

// What the generator does produce here is the sentinel it promises for exactly
// this family: the call's result carries a loan whose AccessPath is `Unknown` --
// "this borrow came from a construct I could not follow". The InvalidateOrigin
// fact for push_back IS emitted; the receiver's only loan is that Unknown one, and
// `AccessPath::isPrefixOf` compares kind and root, so an Unknown path matched
// NOTHING. The invalidation invalidated nothing and the reallocation went
// unreported, leaving the borrow neither tracked nor refused.
//
// An Unknown path means the mutation may reach any storage, so it must match.
void lost_receiver(std::vector<int> *q [[clang::noescape]]) { // expected-warning {{parameter is later invalidated}}
  int *p = &(*q)[0];
  sysmax(q, q)->push_back(99); // expected-note {{invalidated here}}
  sink = *p;                   // expected-note {{later used here}}
}

// KNOWN IMPRECISION, pinned deliberately: because an Unknown path carries no
// information to discriminate on, EVERY live borrow at such a call is reported,
// including one that demonstrably cannot alias the mutated vector. Any filter here
// would have to invent knowledge the Unknown path says we do not have, so the
// conservative report stands. It is confined to calls whose receiver was lost this
// way -- rare in practice, and it costs nothing when no borrow is live.
void unrelated_borrow_also_reported(std::vector<int> *q [[clang::noescape]],
                                    int *outside [[clang::noescape]]) { // expected-warning {{parameter is later invalidated}}
  sysmax(q, q)->push_back(99);                                         // expected-note {{invalidated here}}
  sink = *outside;                                                     // expected-note {{later used here}}
}

// No live borrow, so nothing is reported: the Unknown receiver on its own is not
// an error, which is what keeps this narrow.
void no_live_borrow(std::vector<int> *q [[clang::noescape]]) {
  sysmax(q, q)->push_back(99);
}

// The same lost receiver with an ASSUMED invalidation -- a non-const method
// whose body the analysis does not see -- went silent: the assumed check had no
// Unknown rule, so the receiver's only loan invalidated nothing while `q->grow()`
// was reported.
void assumed_through_lost_receiver(W *q [[clang::noescape]]) { // expected-warning {{parameter may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  const int *p = q->first();
  sysmaxW(q, q)->grow(); // expected-note {{assumed to be invalidated by this operation}}
  sink = *p;
}
