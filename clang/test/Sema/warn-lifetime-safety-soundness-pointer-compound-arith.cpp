// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

// Compound assignment (`p += n` / `p -= n`) and pre/post increment-decrement
// (`++p`, `--p`, `p++`, `p--`) on a pointer keep it aimed into the same
// allocation, so the result carries the operand's loans. Previously these forms
// dropped the loan: a borrow used via the result expression (e.g. escaping to a
// global) was silently lost. The loan now propagates, so the escape is caught.
// (The dangling-global diagnostic anchors at the borrow, i.e. the `int *p = ...`
// initialization.)

int *g; // expected-note 5 {{this global dangles}}

void via_compound_add() {
  int local[10];
  int *p = local; // expected-warning {{stack memory associated with local variable 'local' escapes to the global variable 'g' which will dangle}}
  g = (p += 1);
}

void via_compound_sub() {
  int local[10];
  int *p = local + 5; // expected-warning {{escapes to the global variable 'g'}}
  g = (p -= 1);
}

void via_preinc() {
  int local[10];
  int *p = local; // expected-warning {{escapes to the global variable 'g'}}
  g = ++p;
}

void via_postinc() {
  int local[10];
  int *p = local; // expected-warning {{escapes to the global variable 'g'}}
  g = p++;
}

void via_predec() {
  int local[10];
  int *p = local + 5; // expected-warning {{escapes to the global variable 'g'}}
  g = --p;
}

// Negative: arithmetic on a pointer into long-lived (global/static) storage does
// not dangle and stays silent.
void ok_global_storage() {
  static int s[10];
  int *p = s;
  p += 1;
  ++p;
  g = (p -= 1); // no-warning
}
