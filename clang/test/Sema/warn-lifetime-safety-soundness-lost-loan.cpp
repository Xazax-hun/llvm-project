// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-lost-loan -verify=expected %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify=expected,umbrella %s

// The lost-loan soundness warning fires when the analysis tracks a
// pointer-like value but holds no borrow for it, i.e. a loan was lost because
// some construct was not modeled (or the value is null/uninitialized and thus
// untracked). It is part of the "safe programming model" soundness group.
//
// The second RUN enables the whole -soundness umbrella, which additionally
// flags the unannotated indirection in each 'use' call (and the unannotated
// parameter 'q'); those are checked under the 'umbrella' prefix.

void use(int *p);
int *make(); // No lifetime annotations: the analysis cannot model its result.

// A [[clang::lifetime_immortal]] function returns storage that lives forever,
// so its result carries a tracked (immortal) borrow and is never "lost".
[[clang::lifetime_immortal]] int *get_immortal();

// A borrow the analysis tracks end-to-end: no lost loan.
void tracked() {
  int x;
  int *p = &x;
  use(p); // umbrella-warning {{argument is bound to a parameter that can hold a borrow but is not annotated for lifetime safety}}
}

// The result of an immortal function is tracked: no lost loan.
void immortal_is_tracked() {
  int *p = get_immortal();
  use(p); // umbrella-warning {{argument is bound to a parameter that can hold a borrow}}
}

// The result of an unannotated call carries no loan: the borrow is lost.
void lost_from_unmodeled_call() {
  int *p = make();
  use(p); // expected-warning {{lifetime safety cannot track local variable 'p' here; no borrow information flows into it, so a borrow was likely lost to an unmodeled construct}} umbrella-warning {{argument is bound to a parameter that can hold a borrow}}
}

// A null/uninitialized pointer is untracked by the loan model.
void null_is_untracked() {
  int *p = nullptr;
  use(p); // expected-warning {{lifetime safety cannot track local variable 'p' here}} umbrella-warning {{argument is bound to a parameter that can hold a borrow}}
}

// A use whose only operand is the unmodeled call itself.
void direct_unmodeled_arg() {
  use(make()); // expected-warning {{lifetime safety cannot track this value here}} umbrella-warning {{argument is bound to a parameter that can hold a borrow}}
}

// Pointer parameters receive a placeholder loan, so they are tracked.
void param_has_placeholder_loan(int *q) { // umbrella-warning {{parameter that can hold a borrow is not annotated for lifetime safety}}
  use(q); // umbrella-warning {{argument is bound to a parameter that can hold a borrow}}
}

// Taking the address of a global yields a tracked loan.
void address_of_global() {
  static int g;
  int *p = &g;
  use(p); // umbrella-warning {{argument is bound to a parameter that can hold a borrow}}
}
