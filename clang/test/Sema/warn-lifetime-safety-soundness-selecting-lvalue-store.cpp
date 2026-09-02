// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wno-gnu-conditional-omitted-operand -Wlifetime-safety-soundness -verify %s

// Soundness companion to warn-lifetime-safety-soundness-unsupported-store.cpp and
// its array-subscript sibling. Those check that a store through a selecting
// lvalue is no longer REFUSED; these check that it is actually TRACKED -- a borrow
// of a dying local stored through each spelling must still be reported, and
// through EVERY candidate destination, since which one was written is unknown.
//
// The pointers start null, so reading them as the selecting arms finds no borrow
// and reports a lost loan at the store; that is unrelated to the routing and is
// expected here.

volatile int sink;
int side();

void cond(bool c) {
  int *p = nullptr, *q = nullptr;
  {
    int local = 5;
    // expected-warning@+3 {{cannot track local variable 'p'}}
    // expected-warning@+2 {{cannot track local variable 'q'}}
    // expected-warning@+1 {{local variable 'local' does not live long enough}}
    (c ? p : q) = &local;
  } // expected-note {{destroyed here}}
  sink = *p + *q; // expected-note {{later used here}}
}

void comma() {
  int *p = nullptr;
  {
    int local = 5;
    // expected-warning@+2 {{cannot track local variable 'p'}}
    // expected-warning@+1 {{local variable 'local' does not live long enough}}
    (side(), p) = &local;
  } // expected-note {{destroyed here}}
  sink = *p; // expected-note {{later used here}}
}

void gnu_cond(bool c) {
  int *p = nullptr, *q = nullptr;
  {
    int local = 5;
    // expected-warning@+3 {{cannot track local variable 'p'}}
    // expected-warning@+2 {{cannot track local variable 'q'}}
    // expected-warning@+1 {{local variable 'local' does not live long enough}}
    (p ?: q) = &local;
  } // expected-note {{destroyed here}}
  sink = *p + *q; // expected-note {{later used here}}
}

// The array-subscript form: the selecting BASE names the real arrays.
void array_cond_base(bool c) {
  int *a[4] = {};
  int *b[4] = {};
  {
    int local = 0;
    // expected-warning@+3 {{cannot track local variable 'a'}}
    // expected-warning@+2 {{cannot track local variable 'b'}}
    // expected-warning@+1 {{local variable 'local' does not live long enough}}
    (c ? a : b)[2] = &local;
  } // expected-note {{destroyed here}}
  sink = *a[2] + *b[2]; // expected-note {{later used here}}
}

// Through a parameter: the destination is named by a placeholder loan, which
// resolves to the parameter's origin.
void through_param(bool c, int **x [[clang::noescape]], // expected-warning {{uses more than one level of indirection}}
                   int **y [[clang::noescape]]) {       // expected-warning {{uses more than one level of indirection}}
  {
    int local = 0;
    *(c ? x : y) = &local; // expected-warning {{local variable 'local' does not live long enough}}
  }                        // expected-note {{destroyed here}}
  sink = **x; // expected-note {{later used here}}
}
