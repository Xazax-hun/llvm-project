// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

// An array whose element type is itself an indirection (a pointer, reference, or
// view) decays to a pointer-to-pointer -- a double level of indirection the
// analysis cannot model (like `int**` / `&p`, and std::vector<int*> being
// rejected). The decay is rejected EXCEPT as the immediate base of an `arr[i]`
// subscript, which is a single-level element access that is modeled. So `arr[i]`
// stays usable, while escaping the decay as a bare pointer-to-pointer is flagged.

void take(int **pp);

// Escaping decays of an array-of-pointers (the bypass shapes): flagged.
void deref_conditional(bool c) {
  int *arr[2];
  int *arr2[2];
  *(c ? arr : arr2) = nullptr; // expected-warning {{this array of pointers (decaying to a pointer to a pointer) uses more than one level of indirection}}
}

void deref_arith() {
  int *arr[2];
  *(arr + 1) = nullptr; // expected-warning {{array of pointers (decaying to a pointer to a pointer)}}
}

void pass_as_ptr_to_ptr() {
  int *arr[2];
  take(arr); // expected-warning {{array of pointers (decaying to a pointer to a pointer)}}
}

// `arr[i]` subscript element access stays usable (single level): no warning.
void subscript_ok() {
  int *arr[2];
  int x = 0;
  arr[0] = &x; // no-warning
  int *q = arr[1]; // no-warning
  (void)q;
}

// Arrays of non-indirection elements decay fine.
void scalar_array_ok() {
  int arr[4];
  int *p = arr; // no-warning: element type is `int`, not an indirection
  (void)p;
  char buf[8];
  const char *s = buf; // no-warning
  (void)s;
}
