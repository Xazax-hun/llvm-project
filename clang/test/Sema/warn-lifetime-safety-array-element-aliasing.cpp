// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-use-after-scope -verify %s

// An array of pointer-like elements shares one element-origin (indices are not
// disambiguated). A borrow stored into an element via a subscript `arr[i]` and
// via the equivalent decayed-pointer dereference `*(arr+i)` denote the same
// storage, so both stores must be observed at any element read.

int g = 7;

void store_subscript_read_subscript() {
  int *arr[3] = {&g, &g, &g};
  {
    int local = 1;
    arr[1] = &local; // expected-warning {{does not live long enough}}
  }                  // expected-note {{destroyed here}}
  (void)*arr[1];     // expected-note {{later used here}}
}

void store_ptrarith_read_subscript() {
  int *arr[3] = {&g, &g, &g};
  {
    int local = 1;
    *(arr + 1) = &local; // expected-warning {{does not live long enough}}
  }                      // expected-note {{destroyed here}}
  (void)*arr[1];         // expected-note {{later used here}}
}

void store_subscript_read_ptrarith() {
  int *arr[3] = {&g, &g, &g};
  {
    int local = 1;
    arr[1] = &local; // expected-warning {{does not live long enough}}
  }                  // expected-note {{destroyed here}}
  (void)**(arr + 1); // expected-note {{later used here}}
}

// Nested arrays: the inner array object's element-origin is shared.
void nested_array() {
  int *m[2][2] = {{&g, &g}, {&g, &g}};
  {
    int local = 1;
    *(m[1] + 0) = &local; // expected-warning {{does not live long enough}}
  }                       // expected-note {{destroyed here}}
  (void)*m[1][0];         // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

int g2 = 8;

// Storing a long-lived address through pointer arithmetic is fine.
void store_global_ptrarith() {
  int *arr[3] = {&g, &g, &g};
  *(arr + 1) = &g2;
  (void)*arr[1]; // no-warning
}

// A subscript/deref of a real pointer (not an array object) is an ordinary
// indirection, not a shared array element.
void pointer_param(int **p) {
  int local = 1;
  *(p + 1) = &local; // no-warning (p is a pointer, not an array)
  (void)p;
}
