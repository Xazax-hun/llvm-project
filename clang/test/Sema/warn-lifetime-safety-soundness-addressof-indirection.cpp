// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-multilevel-indirection -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// The safe programming model supports only a single level of indirection. The
// declaration-level rule (e.g. 'int **') is mirrored for transient expressions:
// taking the address of an indirection (a pointer or a view) forms a second
// level that the analysis cannot fully model. This closes store-through-
// dereference holes such as '*&sv = q' and '*(c ? &a : &b) = q', whose double
// indirection no declaration captures.

string g = "global backing content long enough to matter";

void addressof_view(string_view sv) {
  (void)&sv; // expected-warning {{uses more than one level of indirection}}
}

void addressof_pointer(int *p) {
  (void)&p; // expected-warning {{uses more than one level of indirection}}
}

// The store-through-dereference exploit is rejected because it forms '&sv'.
void deref_store() {
  string_view sv = g;
  string local = g;
  *&sv = local; // expected-warning {{uses more than one level of indirection}}
  (void)sv;
}

// The computed-pointer variant is likewise rejected (it forms '&a'/'&b').
void deref_store_ternary(bool c) {
  string_view a = g, b = g;
  string local = g;
  *(c ? &a : &b) = local; // expected-warning 2 {{uses more than one level of indirection}}
  (void)a;
  (void)b;
}

//===----------------------------------------------------------------------===//
// Negatives: taking the address of a non-indirection is a single level.
//===----------------------------------------------------------------------===//

void addressof_owner(string s) {
  void sink(string *);
  sink(&s); // no-warning (address of an owner)
}

void addressof_scalar(int x) {
  void sink(int *);
  sink(&x); // no-warning (address of a scalar)
}
