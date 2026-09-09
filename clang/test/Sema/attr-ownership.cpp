// RUN: %clang_cc1 %s -verify -fsyntax-only

class C {
  void *f(int, int)
       __attribute__((ownership_returns(foo, 2)))  // expected-error {{'ownership_returns' attribute index does not match; here it is 2}}
       __attribute__((ownership_returns(foo, 3))); // expected-note {{declared with index 3 here}}
};

// `ownership_takes` may name the implicit object parameter of a non-static member
// function (index 1): the call deallocates the object it is called on. The other
// two kinds may not -- `ownership_holds` has no meaning for the object the callee
// is already called on, and `ownership_returns` indexes an allocation size.
struct ImplicitObject {
  void takes_this() __attribute__((ownership_takes(RC, 1)));           // OK
  void takes_this_const() const __attribute__((ownership_takes(RC, 1))); // OK
  void takes_param(void *p) __attribute__((ownership_takes(malloc, 2))); // OK

  // expected-error@+1 {{'ownership_holds' attribute is invalid for the implicit this argument}}
  void holds_this() __attribute__((ownership_holds(RC, 1)));
  // expected-error@+1 {{'ownership_returns' attribute is invalid for the implicit this argument}}
  void *returns_this() __attribute__((ownership_returns(RC, 1)));

  // A static member function has no implicit object parameter, so index 1 is its
  // first declared parameter.
  static void takes_first(void *p) __attribute__((ownership_takes(malloc, 1))); // OK

  // Still out of bounds.
  // expected-error@+1 {{'ownership_takes' attribute parameter 1 is out of bounds}}
  void takes_oob() __attribute__((ownership_takes(RC, 2)));
};
