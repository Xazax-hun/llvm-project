// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wno-dangling -Wlifetime-safety-soundness -verify %s

// A read of a structured-binding element registers a use of the decomposed
// object, so a borrow the decomposed object holds stays live at the read and an
// expiry / free / invalidation of the source is reported. A scalar binding
// element (e.g. `int a`) has no origin of its own, so without this the read of
// `a` would not register against the source's borrow and a use-after-free /
// use-after-scope through the element would be missed. (-Wno-dangling so the
// legacy temporary-dangling warning does not mask what the soundness group sees.)

struct P {
  int a;
  int b;
};

int sink;

// Use-after-free: bind by reference to a heap object, free it, then read an
// element. (Was a silent bypass: no warning of any kind.)
void heap_uaf() {
  P *h = new P{1, 2};      // expected-warning {{allocated object does not live long enough}}
  const auto &[a, b] = *h;
  delete h;                // expected-note {{freed here}}
  sink = a;                // expected-note {{later used here}}
}

// Use-after-scope: bind by reference to a dangling temporary.
const P &pick(const P &p [[clang::lifetimebound]]) { return p; }
void scope_uas() {
  const auto &[a, b] = pick(P{1, 2}); // expected-warning {{local temporary object does not live long enough}} expected-note {{destroyed here}}
  sink = a;                           // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A by-value decomposition is an independent copy; reading it after the source
// is freed is fine.
void by_value_copy() {
  P *h = new P{1, 2};
  auto [a, b] = *h;
  delete h;
  sink = a; // no-warning (reads the copy)
}

// Binding a persistent object.
P g_p{1, 2};
void persistent() {
  auto &[a, b] = g_p;
  sink = a; // no-warning
}

// In-scope read.
void in_scope() {
  P p{1, 2};
  auto &[a, b] = p;
  sink = a + b; // no-warning
}
