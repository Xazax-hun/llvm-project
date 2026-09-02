// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A store into an array-of-pointers element whose subscript BASE selects/forwards
// among arrays -- `(c ? a : b)[i] = &local`, `(f(), a)[i] = ...` -- routed to the
// transient element-origin of the selecting expression, which a later element
// read of the real arrays never re-resolved to, so the borrow was dropped and the
// store was rejected wholesale. The destinations now come from the loans the
// selecting base holds, which name the real arrays, so these are TRACKED. This is
// the array-subscript sibling of the scalar `(c ? p : q) = ...` case.

int g_side();

// Conditional-selected array base: rejected.
void cond_base(bool c) {
  int *a[4];
  int *b[4];
  int local = 0;
  (c ? a : b)[2] = &local; // no-warning: routed to a and b
}

// Comma-selected array base: rejected.
void comma_base() {
  int *a[4];
  int local = 0;
  (g_side(), a)[1] = &local; // no-warning: routed
}

// Reversed subscript `i[c?a:b]` (== `(c?a:b)[i]`): rejected.
void reversed_subscript(bool c) {
  int *a[4];
  int *b[4];
  int local = 0;
  2[c ? a : b] = &local; // no-warning: routed
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
