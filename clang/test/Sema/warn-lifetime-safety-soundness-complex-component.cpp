// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// `__real__ obj` / `__imag__ obj` name a component of obj (a glvalue when obj is
// one), so `&__real__ obj` borrows obj's storage. VisitUnaryOperator handled
// only &, *, and ++/--, so UO_Real/UO_Imag fell into the default case and flowed
// no loan -- the address-of yielded an empty origin and the borrow was dropped.
// The operand's origin is now propagated.

int *ret_real() {
  _Complex int c = 0;
  return &__real__ c; // expected-warning {{stack memory associated with local variable 'c' is returned}} expected-note {{returned here}}
}

int *ret_imag() {
  _Complex int c = 0;
  return &__imag__ c; // expected-warning {{stack memory associated with local variable 'c' is returned}} expected-note {{returned here}}
}

// __real__ is the identity on a real operand; &__real__ x == &x.
int *ret_real_scalar() {
  int x = 0;
  return &__real__ x; // expected-warning {{stack memory associated with local variable 'x' is returned}} expected-note {{returned here}}
}

// Negative: a long-lived component borrow stays silent.
int *ret_real_static() {
  static _Complex int c = 0;
  return &__real__ c; // no-warning
}
