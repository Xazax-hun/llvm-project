// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// The unknown-ownership / owner-of-indirection / pointer-of-indirection checks
// bailed out on array types (getAsCXXRecordDecl() is null for an array), so a
// local array of a borrow-holding element was flagged at neither its declaration
// nor any element access -- the element borrow was dropped and the array's
// Uninitialized element-origin sentinel kept lost-loan silent too. Array
// dimensions are now peeled so `P a[N]` is flagged like the scalar `P a`.

struct P {
  const int *p;
};

// Array of a plain borrow-holding struct: flagged at the declaration like `P a;`.
void array_of_unknown() {
  P a[1] = {{nullptr}}; // expected-warning {{can hold a borrow but is annotated neither}}
  (void)a;
}

// Multi-dimensional too.
void array2d_of_unknown() {
  P a[2][2] = {}; // expected-warning {{can hold a borrow but is annotated neither}}
  (void)a;
}

// A custom [[gsl::Owner]] whose element is an indirection, as an array element:
// flagged as owner-of-indirection (peeled like the scalar form).
template <class T> struct [[gsl::Owner]] Bag {
  T *data; // expected-warning {{public data member 'data' of a [[gsl::Owner]] type can hold a borrow}} expected-warning {{field 'data' uses more than one level of indirection}}
};
void array_of_owner_of_indirection() {
  Bag<int *> a[1]; // expected-warning {{element type holds a borrow}} \
                   // expected-note {{in instantiation of template class 'Bag<int *>' requested here}}
  (void)a;         // expected-warning {{lifetime safety cannot track}}
}

// Scalar control: still flagged (unchanged).
void scalar_unknown() {
  P a; // expected-warning {{can hold a borrow but is annotated neither}}
  (void)a;
}
