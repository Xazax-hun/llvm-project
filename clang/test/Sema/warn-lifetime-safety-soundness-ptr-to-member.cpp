// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// Pointer-to-data-member access `obj.*pm` (and `objptr->*pm`) names a member of
// the object, so `&(obj.*pm)` borrows the object. These operators were not
// modeled (no BO_PtrMemD/BO_PtrMemI case in VisitBinaryOperator), so the borrow
// of the object was dropped to an empty origin. Normally lost-loan catches that,
// but a control-flow merge supplying a valid loan on the other path masks it --
// a silent use-after-scope. The result now flows from the object operand.

int g = 1;
struct S {
  int x;
};
int sink;

// `.*` form: the dropped borrow of `s` is masked by the `&g` loan from the
// other path.
int via_dot_star(bool c) {
  const int *out = &g;
  if (c) {
    S s{5};
    int S::*pm = &S::x;
    out = &(s.*pm); // expected-warning {{'s' does not live long enough}}
  }                 // expected-note {{destroyed here}}
  sink = *out;      // expected-note {{later used here}}
  return sink;
}

// `->*` form through a pointer to the object.
int via_arrow_star(bool c) {
  const int *out = &g;
  if (c) {
    S s{5};
    int S::*pm = &S::x;
    S *sp = &s;       // expected-warning {{'s' does not live long enough}}
    out = &(sp->*pm); // borrow of s via ->*
  } // expected-note {{destroyed here}}
  sink = *out; // expected-note {{later used here}}
  return sink;
}

// Negative: a long-lived object (static) borrowed through `.*` stays silent.
int via_dot_star_ok(bool c) {
  static S s{5};
  const int *out = &g;
  if (c) {
    int S::*pm = &S::x;
    out = &(s.*pm);
  }
  sink = *out; // no-warning
  return sink;
}
