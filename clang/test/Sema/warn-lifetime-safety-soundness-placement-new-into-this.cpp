// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A placement new constructs into storage that already EXISTS and outlives the
// expression, so a borrow the new object captures comes to rest there. The
// initializer flowed only into the new-expression's own pointee origin, which for
// a placement form is a throwaway -- so `new (this) S{t}` left the borrow of `t`
// in it and the object never received it, while `v = t` and `*this = S{t}` in the
// same class were both reported. The store is now routed by the loans the buffer
// holds, as any other store through an lvalue is.

volatile char sink;

struct [[gsl::Pointer]] S {
  string_view v; // expected-note {{this field dangles}}

  // Reported all along: a direct field store.
  void assign_field() {
    string t = "a long heap string value exceeding the sso buffer now";
    v = t; // expected-warning {{stack memory associated with local variable 't' escapes to the field}}
  }

  // Reported all along: whole-object assignment.
  void assign_object() {
    string t = "a long heap string value exceeding the sso buffer now";
    *this = S{t}; // expected-warning {{local variable 't' does not live long enough}}
  } // expected-note {{destroyed here}} expected-note {{later used here}}

  // Was silent: same class, same local, only the spelling differs.
  void placement_into_this() {
    string t = "a long heap string value exceeding the sso buffer now";
    new (this) S{t}; // expected-warning {{local variable 't' does not live long enough}}
  } // expected-note {{destroyed here}} expected-note {{later used here}}
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// The borrow outlives the object it is placed into.
void placement_outlives(string &keeper) {
  S s;
  new (&s) S{keeper}; // no-warning
  sink = s.v.data()[0];
}

// Nothing borrowed at all.
void placement_no_borrow() {
  S s;
  new (&s) S{}; // no-warning
  (void)s;
}
