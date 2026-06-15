// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A store into an array-of-pointers element whose subscript BASE selects/forwards
// among arrays -- `(c ? a : b)[i] = &local`, `(f(), a)[i] = ...` -- routed to the
// transient element-origin of the selecting expression, which a later element
// read of the real arrays never re-resolves to: the borrow was silently dropped
// (and, with the arrays uninitialized, the lost-loan backstop was masked by the
// Uninitialized sentinel). Such an unroutable store is now rejected. This is the
// array-subscript sibling of the scalar `(c ? p : q) = ...` selecting-lvalue case.

int g_side();

// Conditional-selected array base: rejected.
void cond_base(bool c) {
  int *a[4];
  int *b[4];
  int local = 0;
  (c ? a : b)[2] = &local; // expected-warning {{assignment through this expression is not modeled by lifetime safety analysis}}
}

// Comma-selected array base: rejected.
void comma_base() {
  int *a[4];
  int local = 0;
  (g_side(), a)[1] = &local; // expected-warning {{assignment through this expression is not modeled}}
}

// Reversed subscript `i[c?a:b]` (== `(c?a:b)[i]`): rejected.
void reversed_subscript(bool c) {
  int *a[4];
  int *b[4];
  int local = 0;
  2[c ? a : b] = &local; // expected-warning {{assignment through this expression is not modeled}}
}

// Control: a plain (stable) array base is tracked precisely, not rejected.
void plain_array_tracked() {
  int *a[4];
  {
    int local = 0;
    a[2] = &local; // expected-warning {{'local' does not live long enough}}
  }                // expected-note {{destroyed here}}
  (void)*a[2];     // expected-note {{later used here}}
}

// Control: a long-lived borrow into a plain array stays silent.
void plain_array_ok() {
  static int s = 0;
  int *a[4];
  a[2] = &s;   // no-warning
  (void)*a[2];
}
