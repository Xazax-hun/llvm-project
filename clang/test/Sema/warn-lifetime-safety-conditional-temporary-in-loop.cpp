// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-permissive -verify %s

#include "Inputs/lifetime-analysis.h"

volatile char sink;

//===----------------------------------------------------------------------===//
// A temporary that is only CONDITIONALLY constructed, inside a loop.
//
// The full-expression cleanup -- where the temporary's destructor is modelled as
// a use of the object -- sits in the block where the short circuit MERGES, which
// is reachable without the temporary ever being built. So the backward liveness
// from that destructor-use travelled around the loop back-edge, bypassing the
// Issue in the constructing branch that would have killed it, and the temporary
// was reported as still borrowed at its own destruction: "destroyed here" and
// "later used here" both pointed at the cleanup.
//
// Same merge-block liveness leak that #205740 fixed for the conditional
// operator's origin flows. The cure here is to expire the temporary's ORIGIN and
// not only its access path, exactly as handleLifetimeEnds already does for a
// named local ("to ensure liveness doesn't persist through loop back-edges").
//===----------------------------------------------------------------------===//

struct Result {
  int *m_p;
  bool found;
  Result(int *p, bool f) : m_p(p), found(f) {}
  ~Result() {} // non-trivial: this is what puts a destructor in the CFG
};

struct Table {
  int m_x;
  Result lookup() { return Result{&m_x, true}; }
};

bool short_circuit_or(int count, bool condition) {
  Table table;
  for (int i = 0; i < count; ++i)
    if (condition || !table.lookup().found)
      continue;
  return true;
}

bool short_circuit_and(int count, bool condition) {
  Table table;
  for (int i = 0; i < count; ++i)
    if (condition && !table.lookup().found)
      continue;
  return true;
}

bool conditional_operator(int count, bool condition) {
  Table table;
  for (int i = 0; i < count; ++i)
    if (condition ? true : !table.lookup().found)
      continue;
  return true;
}

// The temporary built inline rather than returned from a call.
bool built_inline(int count, bool condition) {
  Table table;
  for (int i = 0; i < count; ++i)
    if (condition || !Result{&table.m_x, true}.found)
      continue;
  return true;
}

// Passed to a call, so the temporary is consumed rather than read.
bool takes_result(const Result &);
bool passed_to_call(int count, bool condition) {
  Table table;
  for (int i = 0; i < count; ++i)
    if (condition || takes_result(table.lookup()))
      continue;
  return true;
}

// Unconditional in a loop: the Issue and the cleanup share a block, so this was
// always fine. Kept so a future change cannot quietly break it instead.
bool unconditional(int count) {
  Table table;
  for (int i = 0; i < count; ++i)
    (void)table.lookup().found;
  return true;
}

//===----------------------------------------------------------------------===//
// Expiring the origin must not hide a borrow that genuinely OUTLIVES the
// temporary. Nothing can read a destroyed object, so killing its own origin is
// safe; a borrow that survives lives in a different origin, which is untouched.
//===----------------------------------------------------------------------===//

void borrow_outlives_temporary() {
  // expected-warning@+2 {{object backing the pointer will be destroyed at the end of the full-expression}}
  // expected-warning@+1 {{local temporary object does not live long enough}} expected-note@+1 {{destroyed here}}
  const char *p = std::string("a long enough string to heap allocate").c_str();
  sink = *p; // expected-note {{later used here}}
}

// ...including a borrow taken inside the same conditional-in-a-loop shape and
// escaping past the owner it came from.
const char *keep_across_loop(int count, bool condition) {
  const char *keep = nullptr;
  std::string s("a long enough string to heap allocate");
  for (int i = 0; i < count; ++i)
    if (condition || i > 0)
      keep = s.c_str(); // expected-warning {{stack memory associated with local variable 's' is returned}}
  return keep;          // expected-note {{returned here}}
}
