// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// `a.operator=(b)` is the same store as `a = b`, just spelled out. The operator
// syntax is a CXXOperatorCallExpr and reached the assignment modelling; the explicit
// spelling is an ordinary member call and did not, so it deposited nothing at all --
// no FieldStore, no DynamicStore. The borrow was simply lost, and the only thing left
// was the lost-borrow sentinel (a refusal), where the operator spelling gave a
// precise report.

volatile char sink;

//===----------------------------------------------------------------------===//
// Both spellings must report the same dangle.
//===----------------------------------------------------------------------===//

void operator_syntax() {
  string_view v;
  {
    string s("temporary");
    v = s; // expected-warning {{local variable 's' does not live long enough}}
  } // expected-note {{destroyed here}}
  sink = v.data()[0]; // expected-note {{later used here}}
}

// The reported shape.
void explicit_spelling() {
  string_view v;
  {
    string s("temporary");
    v.operator=(s); // expected-warning {{local variable 's' does not live long enough}}
  } // expected-note {{destroyed here}}
  sink = v.data()[0]; // expected-note {{later used here}}
}

// Into a member, which is how it was found: an owner whose destructor reads a view
// member it was assigned through the explicit spelling.
struct [[gsl::Owner(char)]] W {
  ~W() { sink = v.data()[0]; }
  friend void member_operator_syntax();
  friend void member_explicit_spelling();

private:
  string_view v;
};

void member_operator_syntax() {
  W w;
  string s("temporary");
  w.v = s; // expected-warning {{local variable 's' does not live long enough}}
} // expected-note {{destroyed here}} expected-note {{later used here}}

void member_explicit_spelling() {
  W w;
  string s("temporary");
  w.v.operator=(s); // expected-warning {{local variable 's' does not live long enough}}
} // expected-note {{destroyed here}} expected-note {{later used here}}

//===----------------------------------------------------------------------===//
// Must stay silent: the assigned-from object outlives the destination.
//===----------------------------------------------------------------------===//

static const char kImmortal[] = "immortal";

void explicit_spelling_safe() {
  string_view v;
  v.operator=(string_view(kImmortal)); // no-warning
  sink = v.data()[0];
}

// A non-assignment operator spelled explicitly is not a store and must not be
// modelled as one.
struct Counter {
  int n = 0;
  Counter &operator+=(int d) {
    n += d;
    return *this;
  }
};

void explicit_other_operator() {
  Counter c;
  c.operator+=(1); // no-warning
  sink = (char)c.n;
}
