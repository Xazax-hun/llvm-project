// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A lambda that captures `[this]` stores a borrow of the enclosing object into
// the closure. That capture was not modeled, so an escaping this-capturing
// closure (returned by value) carried no object loan -- and a co-captured benign
// loan (a lifetimebound parameter) kept the closure origin non-empty, masking
// lost-loan, while the dangling `this` read stayed silent. The `[this]` capture
// now flows the `this` origin into the lambda, so the escape is surfaced: an
// unannotated borrow of `this` escaping via return is a soundness issue (the
// intra-TU lifetimebound suggestion is part of the soundness model).

volatile int sink;

struct W {
  int x;

  // Returns a closure capturing `this` (+ a benign lifetimebound-param capture
  // that previously masked the dangling-this). The escape is now flagged.
  auto getter(int &keep [[clang::lifetimebound]]) { // expected-warning {{implicit this in intra-TU function should be marked [[clang::lifetimebound]]}}
    return [this, kp = &keep]() { sink = x; }; // expected-note {{param returned here}}
  }

  // A plain `[this]` capture that escapes is flagged too (no masking needed).
  // The closure return type carries a borrow of `this`, so -- like a pointer
  // return -- it must be annotated; both the requirement and the suggestion fire.
  auto plain() { // expected-warning {{implicit this in intra-TU function should be marked [[clang::lifetimebound]]}} \
                 // expected-warning {{member function returning '(lambda at}}
    return [this]() { sink = x; }; // expected-note {{param returned here}}
  }
};

//===----------------------------------------------------------------------===//
// Negative: a `[this]`-capturing closure used locally (not escaping) is fine.
//===----------------------------------------------------------------------===//
struct WOk {
  int x;
  void use() {
    auto f = [this]() { sink = x; };
    f(); // no-warning: closure does not escape the method
  }
};
