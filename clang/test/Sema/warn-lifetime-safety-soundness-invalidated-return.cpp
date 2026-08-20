// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

using std::vector;

// A borrow that is invalidated and then RETURNED: the caller receives a
// dangling borrow. The analysis detected this all along -- the borrow is live
// across the invalidation and holds the invalidated loan -- but the reporting
// arm for "invalidated, and the escape that keeps it live is a return" was
// unimplemented, so the finding was computed and dropped.
//
// Which spelling reaches that arm is decided by evaluation order. When the
// borrow is READ before the invalidating call and deposited into the returned
// value after it, no Use fact spans the call and the return escape is the only
// thing keeping the loan live. Writing the same thing as two statements keeps a
// Use alive across the call instead, which reported through the use path.

// expected-warning@+1 {{parameter which is returned is invalidated before the function returns}}
const int *read_before_invalidation(vector<int> &v [[clang::lifetimebound]],
                                    unsigned long cap) {
  const int *base = v.data();
  // The comma's left operand runs after `base` is read but before the `+`
  // deposits it into the result.
  return base + (v.resize(cap), 0u); // expected-note {{invalidated here}}
  // expected-note@-1 {{returned here}}
}

// The two-statement spelling of the same body: a Use spans the call, so this
// reports through the use path instead. Both must be reported.
// expected-warning@+1 {{parameter is later invalidated}}
const int *invalidation_before_read(vector<int> &v [[clang::lifetimebound]],
                                    unsigned long cap) {
  const int *base = v.data();
  v.resize(cap); // expected-note {{invalidated here}}
  return base;   // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// No invalidation at all.
const int *no_invalidation(vector<int> &v [[clang::lifetimebound]]) {
  return v.data(); // no-warning
}

// The invalidation is on a DIFFERENT container, so the returned borrow is
// unaffected.
const int *other_container(vector<int> &v [[clang::lifetimebound]],
                           vector<int> &other [[clang::noescape]]) {
  const int *base = v.data();
  other.resize(1024);
  return base; // no-warning
}

// A fresh heap allocation is not a borrow of anything invalidated.
int *fresh_heap() {
  return new int(7); // no-warning
}
