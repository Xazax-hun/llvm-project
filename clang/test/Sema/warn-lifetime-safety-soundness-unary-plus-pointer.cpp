// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

// Unary plus on a pointer is the identity (`+p == p`), so it must carry the
// operand's loans. Previously UO_Plus fell through VisitUnaryOperator's default
// and left the result origin *empty*, which a control-flow merge supplying a
// valid loan on another path masked (suppressing the lost-loan backstop). Now
// the borrow is tracked precisely and caught at the borrowed local's expiry,
// robustly across the merge.

static volatile int sink;
int g = 0;

// No merge: the borrow flows through `+`, tracked as a use-after-scope.
void no_merge() {
  int *p = &g;
  {
    int local = 1;
    p = +&local; // expected-warning {{does not live long enough}}
  } // expected-note {{destroyed here}}
  *p = 42; // expected-note {{later used here}}
  sink = *p;
}

// Masked form: `+&local` as one arm of a `?:` whose other arm holds a valid
// persistent loan (`&g`). An empty origin would be hidden by the union merge;
// the tracked loan is still caught at expiry.
void masked(bool c) {
  int *p = &g;
  {
    int local = 1;
    p = c ? p : +&local; // expected-warning {{does not live long enough}}
  } // expected-note {{destroyed here}}
  *p = 42; // expected-note {{later used here}}
  sink = *p;
}
