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

// Members of the implicit object are caller-provided storage; they are seeded
// with an uninitialized loan at method entry (recursively, through by-value
// sub-objects). Reading an unwritten member -- directly, through an array
// element, or through a nested sub-object -- is therefore not a "lost loan".
struct WithMembers {
  int *scalar;
  int *arr[4];
  struct Inner {
    int *q;
    int *iarr[2];
  } nested;
  void other();

  void read_scalar() {
    int *p = scalar;
    (void)p; // no lost-loan
  }
  void read_array_elem() {
    int *p = arr[0];
    (void)p; // no lost-loan
  }
  void read_nested() {
    int *p = nested.q;
    (void)p; // no lost-loan
  }
  void read_nested_array() {
    int *p = nested.iarr[1];
    (void)p; // no lost-loan
  }
  // Calling another method marks the implicit object's fields as used; with the
  // members seeded this is not a lost loan on 'this'.
  void calls_other() { other(); } // no lost-loan
};

// A pointer materialized from an integer has no tracked provenance: a borrow
// laundered through the integer was dropped. The result is reported as a lost
// loan even when it is consumed directly as an rvalue (dereferenced or indexed)
// without ever binding a named pointer variable.
int deref_int_to_ptr(unsigned long n) {
  return *reinterpret_cast<int *>(n); // expected-warning {{lifetime safety cannot track this value here; no borrow information flows into it, so a borrow was likely lost to an unmodeled construct}}
}

int index_int_to_ptr(unsigned long n) {
  return reinterpret_cast<int *>(n)[3]; // expected-warning {{lifetime safety cannot track this value here}}
}

int cstyle_int_to_ptr(unsigned long n) {
  return *(int *)(n); // expected-warning {{lifetime safety cannot track this value here}}
}

//===----------------------------------------------------------------------===//
// Escaping (returned / stored) untracked borrows. checkLostLoan fires on a
// local *use*; an untracked (Unknown) loan that leaves the function via return
// or a store -- without a local use -- is caught here too, so it cannot defeat
// the downstream annotation checks (e.g. a [[clang::noescape]] parameter
// laundered through an unmodeled call escapes via return). Only an Unknown loan
// from a borrow-returning *call* counts; a default/empty construction
// (`return {};`, `nullptr`) borrows nothing and stays silent.
//===----------------------------------------------------------------------===//

// The result of an unmodeled call is returned directly (no local use).
int *return_unmodeled() {
  return make(); // expected-warning {{lifetime safety cannot track this value here; no borrow information flows into it, so a borrow was likely lost to an unmodeled construct}}
}

// Stored into a field.
struct Holder {
  int *p;
  void set() { p = make(); } // expected-warning {{lifetime safety cannot track this value here}}
};

// Stored into global storage.
int *g_ptr;
void store_global() {
  g_ptr = make(); // expected-warning {{lifetime safety cannot track this value here}}
}

// Negatives: nothing is borrowed, so escaping is safe.
int *return_null() { return nullptr; }                 // no-warning
int *return_tracked() { static int g; return &g; }     // no-warning
int *return_immortal_call() { return get_immortal(); } // no-warning

