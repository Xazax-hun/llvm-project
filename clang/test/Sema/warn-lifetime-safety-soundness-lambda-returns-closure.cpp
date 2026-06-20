// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// Under the safe programming model, a member function (including a lambda's
// operator()) whose return value carries a borrow must annotate where that
// borrow comes from -- [[clang::lifetimebound]] on the implicit object (or a
// parameter) or [[clang::lifetime_immortal]] on the function. This applies not
// only to pointer/reference/view returns but to any return type that holds a
// borrow -- in particular a lambda that returns a closure capturing by
// reference: the returned closure depends on the implicit object's captures.
// Without this, a nested immediately-invoked lambda returning an inner closure
// that shares a by-reference capture was a silent use-after-scope.

int sink;

// The headline: the outer lambda's operator() returns the inner closure, which
// captures `x` by reference -- a borrow-carrying return that must be annotated.
auto nested_iife() {
  int x = 7;
  return [&x] { // expected-warning {{member function returning '(lambda at}}
    return [&x] { return x; };
  }();
}

// The same shape with a named outer closure.
auto named_outer() {
  int x = 1;
  auto outer = [&x] { // expected-warning {{member function returning '(lambda at}}
    return [&x] { return x; };
  };
  return outer();
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A value-returning lambda carries no borrow in its result.
int value_return() {
  int x = 4;
  auto l = [&x] { return x; }; // no-warning
  return l();
}

// A by-value-capture closure returned from a factory holds no borrow, so neither
// the factory nor the returned closure's operator() is flagged.
auto make_adder(int n) {
  return [n](int y) { return y + n; }; // no-warning
}
int use_adder() { return make_adder(5)(3); }

// The borrow-returning operator() is flagged at the lambda definition,
// regardless of how the result is used (just as a pointer-returning method is
// flagged independent of its call sites).
void flagged_at_definition() {
  int x = 3;
  auto l = [&x] { // expected-warning {{member function returning '(lambda at}}
    return [&x] { return x; };
  };
  (void)l()();
}
