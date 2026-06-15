// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-gnu-conditional-omitted-operand -verify %s

// The GNU binary conditional `a ?: b` yields `a` when truthy, else `b`, so its
// result carries the loans of both candidate values. Previously there was no
// VisitBinaryConditionalOperator, so the result carried no loan and a borrow
// used through it could be dropped (and masked by a `?:` sibling).

void use(int *p [[clang::noescape]]);

// A dangling borrow flowing through the true arm of `a ?: b` is caught.
void true_arm() {
  static int valid;
  int *keep = &valid;
  int *r;
  {
    int local = 0;
    int *p = &local; // expected-warning {{does not live long enough}}
    r = p ?: keep;
  } // expected-note {{destroyed here}}
  use(r); // expected-note {{later used here}}
}

// Masked form: `a ?: b` as an arm of an outer `?:` whose other arm is valid.
void masked(bool c) {
  static int valid;
  int *keep = &valid;
  int *r;
  {
    int local = 0;
    int *p = &local; // expected-warning {{does not live long enough}}
    r = c ? keep : (p ?: keep);
  } // expected-note {{destroyed here}}
  use(r); // expected-note {{later used here}}
}

// Negative: both candidates are long-lived -> silent.
void ok() {
  static int a, b;
  int *p = &a;
  int *r = p ?: &b;
  use(r); // no-warning
}
