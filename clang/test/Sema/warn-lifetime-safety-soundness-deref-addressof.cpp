// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

// A dereference of an address-of (`*&E`) denotes the same object as `E`. An
// access through that round-trip must route to the same storage as a plain `E`,
// so a borrow stored through it is tracked. Otherwise the store would land on a
// fresh, disconnected origin and a later dangling read would go unreported.

void use(int);

int g_int;

// Array element store spelled `(*&arr)[i]` (the bug): must be tracked like
// `arr[i]`.
void array_element_store_via_deref_addressof() {
  int *arr[4];
  {
    int local = 1;
    (*&arr)[0] = &local; // expected-warning {{does not live long enough}}
  } // expected-note {{destroyed here}}
  use(*arr[0]); // expected-note {{later used here}}
}

// Control: the plain `arr[i]` spelling is (and was) caught.
void array_element_store_plain() {
  int *arr[4];
  {
    int local = 1;
    arr[0] = &local; // expected-warning {{does not live long enough}}
  } // expected-note {{destroyed here}}
  use(*arr[0]); // expected-note {{later used here}}
}

// Negative: a correct `(*&arr)[i]` round-trip with a long-lived referent stays
// silent (the borrow points at a global that outlives the access).
void no_false_positive() {
  int *arr[4];
  (*&arr)[0] = &g_int; // no-warning
  use(*arr[0]);
}
