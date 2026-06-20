// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A by-reference-capturing closure (or any closure capturing a borrow) wrapped
// in a plain user-defined aggregate holds a borrow. The aggregate's ownership is
// judged from its fields; a closure member's captures must be inspected, because
// isUnknownOwnershipType treats a lambda value as known-safe (it is modeled
// directly when used as a value). Without that, `Box<closure>` looked
// origin-free and an escaping temporary carrying a captured borrow was silently
// dropped -- a use-after-scope no diagnostic caught.

template <class F> struct Box {
  F f;
};
template <class F> struct Inner {
  F f;
};
template <class F> struct Outer {
  Inner<F> in;
};

// Escaping temporary: the returned Box carries the closure's borrow of `x`.
auto return_box() {
  int x = 7;
  auto c = [&x] { return x; };
  return Box<decltype(c)>{c}; // expected-warning {{can hold a borrow but is annotated neither}}
}

// Nested aggregate: the borrow is two levels deep -- both records are untracked.
auto return_nested() {
  int x = 7;
  auto c = [&x] { return x; };
  return Outer<decltype(c)>{Inner<decltype(c)>{c}}; // expected-warning@-0 2 {{can hold a borrow but is annotated neither}}
}

// A `this`-capturing closure wrapped in an aggregate also holds a borrow. The
// method returns it unannotated (flagged at the declaration) and the aggregate
// is untracked (flagged at the return).
struct S {
  int m;
  auto wrap() { // expected-warning {{is not annotated for lifetime safety}}
    auto c = [this] { return m; };
    return Box<decltype(c)>{c}; // expected-warning {{can hold a borrow but is annotated neither}}
  }
};

//===----------------------------------------------------------------------===//
// Negatives: a closure that captures no borrow makes the aggregate borrow-free.
//===----------------------------------------------------------------------===//

// By-value capture of a scalar: no borrow.
auto byval_scalar(int n) {
  auto c = [n] { return n; };
  return Box<decltype(c)>{c}; // no-warning
}

// A capture-less closure: no borrow.
auto no_capture() {
  auto c = [] { return 42; };
  return Box<decltype(c)>{c}; // no-warning
}
