// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A [[gsl::Pointer]] aggregate with BOTH a pointer member and a const& REFERENCE
// member: aggregate-init's per-member loan merge (round-29) skipped the
// reference member because its initializer is the referent glvalue (depth 0),
// not a depth-1 borrow, so getRValueOrigins peeled it away. The dropped
// reference borrow was then masked by the sibling pointer member's (long-lived)
// loan, suppressing lost-loan -- a silent dangle. A reference-member initializer
// now contributes the borrow of its bound lvalue to the leaf object's origin.

struct [[gsl::Pointer(int)]] Mixed {
  const int *p;
  const int &r;
};

int g_long;

const int *braced() {
  const int *held;
  {
    int local = 5;
    Mixed m = {&g_long, local}; // expected-warning {{local variable 'local' does not live long enough}}
    held = &m.r;
  } // expected-note {{destroyed here}}
  return held; // expected-note {{later used here}}
}

const int *designated() {
  const int *held;
  {
    int local = 5;
    Mixed m = {.p = &g_long, .r = local}; // expected-warning {{local variable 'local' does not live long enough}}
    held = &m.r;
  } // expected-note {{destroyed here}}
  return held; // expected-note {{later used here}}
}

// Negative: a reference member bound to a long-lived global is not flagged.
const int *ok() {
  Mixed m = {&g_long, g_long}; // no-warning
  return &m.r;
}
