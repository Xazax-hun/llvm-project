// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A constructor temporary of a borrow-holding non-gsl record (`Box(&x)`, where
// `struct Box { int* p; }` has a capturing constructor) is untracked: the
// captured borrow is dropped (lifetime_capture_by on a constructor is
// unmodeled) and the record's ownership is unknown. A local/member declaration
// of such a type, and a call result, are reported -- but a bare constructor
// temporary that is member-accessed / returned / stored was covered by neither.
// Its only backstop was lost-loan on the dropped borrow, which a control-flow
// merge supplying a valid loan masks -> silent use-after-scope. The constructor
// temporary is now flagged unknown-ownership directly.

struct Box {
  int *p;
  Box(int *q [[clang::lifetime_capture_by(this)]]) : p(q) {}
};

int g_real;
int *gp;

void member_access_of_ctor_temporary(bool c) {
  if (c) {
    int x = 42;
    gp = Box(&x).p; // expected-warning {{type 'Box' can hold a borrow but is annotated neither}}
  } else {
    gp = &g_real; // a valid loan on the other path masks the dropped borrow
  }
  *gp = 7;
}

Box returns_ctor_temporary() {
  int x = 42;
  return Box(&x); // expected-warning {{type 'Box' can hold a borrow but is annotated neither}} \
                  // expected-warning {{address of stack memory associated with local variable 'x' returned}}
}

// Negative: a named declaration is reported once, at the declaration -- not
// again as a temporary (and returning it, a copy/move construct, is not flagged
// a second time).
Box named_then_returned() {
  int x = 42;
  Box b(&x); // expected-warning {{type 'Box' can hold a borrow but is annotated neither}}
  return b;  // no second warning (copy/move construct)
}

// Negative: a non-borrow-holding record temporary is not flagged.
struct Plain {
  int a;
  Plain(int v) : a(v) {}
};
int plain_ctor_temporary() {
  return Plain(7).a; // no-warning
}
