// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// Several borrow-carrying cast kinds were unmodeled by VisitCastExpr (they fell
// into its `default:` and dropped the borrow with no diagnostic), an asymmetry
// against reinterpret_cast (which is caught). `__builtin_bit_cast` of a pointer
// preserves the borrow, and atomic wrap/unwrap (`_Atomic(T*)`) preserves the
// pointer value -- both now propagate the borrow, so a dangling borrow recovered
// through them is caught.

// __builtin_bit_cast of a stack address: caught precisely.
int *bitcast_stack() {
  int x = 0;
  return __builtin_bit_cast(int *, &x); // expected-warning {{stack memory associated with local variable 'x' is returned}} expected-note {{returned here}}
}

// __builtin_bit_cast laundering a pointer out of an integer: the recovered
// pointer has no tracked provenance -> lost loan (same as an int->ptr cast).
int *bitcast_from_int(long n) {
  return __builtin_bit_cast(int *, n); // expected-warning {{lifetime safety cannot track this value here}}
}

// Atomic wrap then unwrap of a stack address: caught precisely.
const int *atomic_stack() {
  int x = 0;
  _Atomic(int *) a = &x; // expected-warning {{stack memory associated with local variable 'x' is returned}}
  return a;              // expected-note {{returned here}}
}

// Negative: a long-lived borrow through an atomic stays silent.
const int *atomic_static() {
  static int s = 0;
  _Atomic(int *) a = &s;
  return a; // no-warning
}

// Negative: a long-lived borrow through bit_cast stays silent (the borrow is
// propagated, and a static's address does not dangle).
int *bitcast_static() {
  static int s = 0;
  return __builtin_bit_cast(int *, &s); // no-warning
}
