// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-gnu-statement-expression -verify %s

// A GNU statement expression (`({ ...; e; })`) yields the value of its final
// expression `e`. Its origin forwards to `e`, and the value is marked used at
// the statement-expression's own program point -- after the body's locals
// expire -- so a borrow `e` carries is tracked: a borrow of a body-local is a
// use-after-scope, and a borrow forwarded from an outer object propagates (and
// can no longer be hidden by a `?:` sibling masking the lost-loan backstop).

void use(int *p [[clang::noescape]]);

// A borrow of a statement-expression-local escaping via the value.
void borrow_of_local() {
  int *p = ({ int x = 7; &x; }); // expected-warning {{local variable 'x' does not live long enough}} expected-note {{destroyed here}} expected-note {{later used here}}
  use(p);
}

// Forwarding an outer borrow that dangles.
void forward_outer_borrow() {
  int *p;
  {
    int local = 0;
    p = ({ (void)0; &local; }); // expected-warning {{local variable 'local' does not live long enough}}
  } // expected-note {{destroyed here}}
  use(p); // expected-note {{later used here}}
}

// Masked by a `?:` sibling: the statement-expression result carries the borrow,
// so the merge no longer hides it.
void masked(bool c) {
  static int valid;
  int *keep = &valid;
  int *r;
  {
    int local = 0;
    r = c ? keep : ({ &local; }); // expected-warning {{local variable 'local' does not live long enough}}
  } // expected-note {{destroyed here}}
  use(r); // expected-note {{later used here}}
}

// Negative: a statement expression yielding a long-lived borrow stays silent.
void ok() {
  static int s;
  int *p = ({ int unused = 0; (void)unused; &s; });
  use(p); // no-warning
}
