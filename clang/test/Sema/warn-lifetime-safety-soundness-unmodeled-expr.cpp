// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// Soundness catch-all: an expression whose type can hold a borrow (a
// pointer/reference/view) but for which the fact generator has no specific
// handler is flagged generically, so the safe programming model never silently
// fails on an unmodeled construct. C11 atomic builtins lower to an AtomicExpr
// node with no handler; the loaded pointer would otherwise carry an empty origin
// and a borrow it should hold would be dropped.

int g;

// Atomic load yielding a pointer: caught by the catch-all.
int *atomic_load_borrow() {
  int x = 0;
  _Atomic(int *) p = &x;
  return __c11_atomic_load(&p, __ATOMIC_RELAXED); // expected-warning {{this expression is not modeled by lifetime safety analysis}}
}

// Negative: an atomic builtin whose result type holds no borrow (an int) is not
// flagged.
int atomic_load_int() {
  _Atomic(int) n = 0;
  return __c11_atomic_load(&n, __ATOMIC_RELAXED); // no-warning
}

// Negative: ordinary handled expressions (a local pointer to a global, a
// conditional) are not swept up by the catch-all.
int *ordinary(bool c) {
  int *q = &g;
  return c ? q : &g; // no-warning
}
