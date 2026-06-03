// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-lost-loan -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-completeness -verify %s

// The lost-loan completeness warning fires when the analysis tracks a
// pointer-like value but holds no borrow for it, i.e. a loan was lost because
// some construct was not modeled (or the value is null/uninitialized and thus
// untracked). It is part of the "safe programming model" completeness group.

void use(int *p);
int *make(); // No lifetime annotations: the analysis cannot model its result.

// A borrow the analysis tracks end-to-end: no warning.
void tracked() {
  int x;
  int *p = &x;
  use(p); // no-warning
}

// The result of an unannotated call carries no loan: the borrow is lost.
void lost_from_unmodeled_call() {
  int *p = make();
  use(p); // expected-warning {{lifetime safety cannot track local variable 'p' here; no borrow information flows into it, so a borrow was likely lost to an unmodeled construct}}
}

// A null/uninitialized pointer is untracked by the loan model.
void null_is_untracked() {
  int *p = nullptr;
  use(p); // expected-warning {{lifetime safety cannot track local variable 'p' here}}
}

// A use whose only operand is the unmodeled call itself.
void direct_unmodeled_arg() {
  use(make()); // expected-warning {{lifetime safety cannot track this value here}}
}

// Pointer parameters receive a placeholder loan, so they are tracked.
void param_has_placeholder_loan(int *q) {
  use(q); // no-warning
}

// Taking the address of a global yields a tracked loan.
void address_of_global() {
  static int g;
  int *p = &g;
  use(p); // no-warning
}
