// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

// A value-preserving explicit reference cast on the destination of an
// assignment (`static_cast<int*&>(p) = ...`, the C-style `(int*&)p = ...`)
// preserves the underlying lvalue, so the store must route to that lvalue's
// storage. Otherwise the borrow is dropped on a disconnected origin and a later
// dangling read goes unreported (a prior valid loan in `p` masks lost-loan).

void use(int);

void store_via_static_cast_ref() {
  int *p = nullptr;
  int valid = 7;
  p = &valid; // a prior valid loan would otherwise mask lost-loan on the read
  {
    int local = 42;
    static_cast<int *&>(p) = &local; // expected-warning {{does not live long enough}}
  } // expected-note {{destroyed here}}
  use(*p); // expected-note {{later used here}}
}

void store_via_cstyle_cast_ref() {
  int *p = nullptr;
  int valid = 7;
  p = &valid;
  {
    int local = 42;
    (int *&)p = &local; // expected-warning {{does not live long enough}}
  } // expected-note {{destroyed here}}
  use(*p); // expected-note {{later used here}}
}

// Control: the plain (uncast) destination is caught.
void store_plain() {
  int *p = nullptr;
  {
    int local = 42;
    p = &local; // expected-warning {{does not live long enough}}
  } // expected-note {{destroyed here}}
  use(*p); // expected-note {{later used here}}
}

// Negative: a value-preserving cast with a long-lived referent stays silent.
int g_int;
void no_false_positive() {
  int *p = nullptr;
  static_cast<int *&>(p) = &g_int; // no-warning
  use(*p);
}
