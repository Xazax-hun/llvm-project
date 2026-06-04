// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-unknown-ownership -verify %s

// A user-defined type that can hold a borrow (has a pointer/reference/
// gsl::Pointer member, transitively) but is annotated neither [[gsl::Owner]]
// nor [[gsl::Pointer]] has ownership the analysis cannot determine.

struct Holder {
  int *p;
};
struct Plain {
  int x, y;
}; // No borrow-capable member: ownership is irrelevant.
struct [[gsl::Owner(int)]] MyOwner {
  int *p;
};
struct [[gsl::Pointer]] MyView {
  int *p;
};
struct Derived : Holder {}; // Inherits a pointer member.
struct Wrapper {
  Holder h;
}; // Contains an unknown-ownership member.

//===----------------------------------------------------------------------===//
// Declarations: parameters.
//===----------------------------------------------------------------------===//

void params(Holder h,    // expected-warning {{type 'Holder' can hold a borrow but is annotated neither [[gsl::Owner]] nor [[gsl::Pointer]], so lifetime safety cannot track its ownership}}
            Plain p,     // no-warning
            MyOwner o,   // no-warning
            MyView v) {  // no-warning
  (void)h;
  (void)p;
  (void)o;
  (void)v;
}

//===----------------------------------------------------------------------===//
// Declarations: local variables.
//===----------------------------------------------------------------------===//

void locals() {
  Holder h;  // expected-warning {{type 'Holder' can hold a borrow}}
  Plain p;   // no-warning
  Derived d; // expected-warning {{type 'Derived' can hold a borrow}}
  Wrapper w; // expected-warning {{type 'Wrapper' can hold a borrow}}
  (void)h;
  (void)p;
  (void)d;
  (void)w;
}

//===----------------------------------------------------------------------===//
// Call return types (no local is produced, e.g. f().foo()).
//===----------------------------------------------------------------------===//

Holder makeHolder();
Plain makePlain();

void calls() {
  makeHolder(); // expected-warning {{type 'Holder' can hold a borrow}}
  makePlain();  // no-warning
}
