// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

// The comma operator's value is its right operand, so the result carries the
// RHS's loans. Previously VisitBinaryOperator had no comma case, so a borrow
// used via a comma result was silently dropped (and could be masked by a valid
// loan merged in from a `?:` sibling).

void use(int *p [[clang::noescape]]);
int side();
int *g; // expected-note {{this global dangles}}

// A dangling borrow flowed through a comma to a global is now caught.
void comma_to_global() {
  int local = 0;
  int *p = &local; // expected-warning {{stack memory associated with local variable 'local' escapes to the global variable 'g' which will dangle}}
  g = (side(), p);
}

// Use-after-scope through a comma result.
void comma_use_after_scope() {
  int *p;
  {
    int local = 0;
    p = (side(), &local); // expected-warning {{does not live long enough}}
  } // expected-note {{destroyed here}}
  use(p); // expected-note {{later used here}}
}

// Masked form: a comma arm of a `?:` whose other arm is a valid loan. The comma
// result now carries the borrow, so the merge no longer hides it.
void comma_masked(bool c) {
  static int valid;
  int *keep = &valid;
  int *r;
  {
    int local = 0;
    r = c ? keep : (side(), &local); // expected-warning {{does not live long enough}}
  } // expected-note {{destroyed here}}
  use(r); // expected-note {{later used here}}
}

// Negative: a comma whose result is a long-lived borrow stays silent.
void comma_ok() {
  static int s;
  int *p = (side(), &s);
  use(p); // no-warning
}
