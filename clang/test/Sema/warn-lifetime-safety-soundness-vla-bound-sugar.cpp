// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -Wno-vla-cxx-extension -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A variable-length array's size expression is evaluated where the type is
// written, and it can carry arbitrary side effects. The CFG collects it by
// casting the variable's type to ArrayType -- which sees nothing when the array
// sits behind type SUGAR, as it does in `__typeof__(char[n])`. The expression
// was then absent from the CFG entirely, so nothing in it was analyzed and a
// whole use-after-free written there was invisible.
//
// The plain spelling `char arr[n]` was always handled, and `typedef`/`using`
// are handled where the type is declared -- so this was one spelling of the
// same construct falling through.

volatile char sink;
volatile int gn;

// The hazard, as one expression, so it can sit in a size position: take a view
// of a heap string, free it, then read the view. Kept local, so what is reported
// is the use-after-free itself rather than an escape to global storage.
#define LOCALS                                                                 \
  string *lp;                                                                  \
  string_view lv
#define HAZARD                                                                 \
  (lp = new string("a long heap string value exceeding the sso buffer"),       \
   lv = string_view(*lp), delete lp, (int)lv.data()[0])

// The reported shape: the array type reached through __typeof__.
void typeof_vla_bound() {
  LOCALS;
  int n = gn;
  __typeof__(char[(HAZARD, n)]) arr; // expected-warning {{allocated object does not live long enough}}
  // expected-note@-1 {{freed here}}
  // expected-note@-2 {{later used here}}
  arr[0] = 1;
  sink = arr[0];
}

// The plain spelling, which was already handled.
void plain_vla_bound() {
  LOCALS;
  int n = gn;
  char arr[(HAZARD, n)]; // expected-warning {{allocated object does not live long enough}}
  // expected-note@-1 {{freed here}}
  // expected-note@-2 {{later used here}}
  arr[0] = 1;
  sink = arr[0];
}

// sizeof of a VLA type evaluates the bound too.
void sizeof_vla_bound() {
  LOCALS;
  int n = gn;
  sink = (char)sizeof(char[(HAZARD, n)]); // expected-warning {{allocated object does not live long enough}}
  // expected-note@-1 {{freed here}}
  // expected-note@-2 {{later used here}}
}

// A typedef evaluates its bound at the TYPE DECLARATION, once. Declaring a
// variable of that type must not re-evaluate it -- looking through typedef
// sugar here would put the expression in the CFG twice and report an
// evaluation that never happens.
void typedef_vla_bound() {
  LOCALS;
  int n = gn;
  typedef char T[(HAZARD, n)]; // expected-warning {{allocated object does not live long enough}}
  // expected-note@-1 {{freed here}}
  // expected-note@-2 {{later used here}}
  T arr;
  arr[0] = 1;
  sink = arr[0];
}

void using_vla_bound() {
  LOCALS;
  int n = gn;
  using T = char[(HAZARD, n)]; // expected-warning {{allocated object does not live long enough}}
  // expected-note@-1 {{freed here}}
  // expected-note@-2 {{later used here}}
  T arr;
  arr[0] = 1;
  sink = arr[0];
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// __typeof__ of an EXPRESSION does not evaluate its operand, so nothing in it
// runs and there is no hazard to report.
void typeof_expression_is_unevaluated() {
  LOCALS;
  int n = gn;
  __typeof__((HAZARD, n)) x = 0; // no-warning
  sink = (char)x;
}

// An ordinary VLA whose bound borrows nothing.
void plain_vla() {
  int n = gn;
  char arr[n + 1]; // no-warning
  arr[0] = 1;
  sink = arr[0];
}

// A bound that reads a live borrow is fine.
void vla_bound_reads_live_borrow() {
  string owner = "a long heap string value exceeding the sso buffer";
  string_view v = owner;
  int n = gn;
  char arr[(v.data()[0] ? n : n + 1)]; // no-warning
  arr[0] = 1;
  sink = arr[0];
}
