// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-assumed-invalidation -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// No alias analysis runs across distinct reference/pointer roots: when two
// reference parameters receive aliasing arguments, a borrow taken from one and a
// mutation through the other are connected only at the call site. Passing the
// same owner to two non-const reference parameters is therefore flagged: the
// callee may borrow one and reallocate the other (== the same object).

void worker(string &a [[clang::noescape]], string &b [[clang::noescape]]);
void worker_const(const string &a [[clang::noescape]], string &b [[clang::noescape]]);
void worker_ptr(string *a [[clang::noescape]], string *b [[clang::noescape]]);
void worker_val(string a, string &b [[clang::noescape]]);

void aliasing_references_warn() {
  string s;
  worker(s, s); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
                // expected-note@-1 {{assumed to be invalidated by this operation}}
}

void aliasing_pointers_warn() {
  string s;
  worker_ptr(&s, &s); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
                      // expected-note@-1 {{assumed to be invalidated by this operation}}
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// Distinct objects do not alias.
void distinct_objects_ok() {
  string x, y;
  worker(x, y); // no-warning
}

// Both parameters const: the call cannot mutate either, so aliasing is harmless.
void both_const_ok() {
  string s;
  void readonly(const string &a [[clang::noescape]],
                const string &b [[clang::noescape]]);
  readonly(s, s); // no-warning
}

// One parameter is by value (a copy): it does not alias the reference parameter.
void by_value_ok() {
  string s;
  worker_val(s, s); // no-warning
}
