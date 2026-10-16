// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// Which arguments are moved into rvalue-reference parameters was decided with an
// offset of one for every instance method, so for a C++23 explicit object member
// function `Args[1]` was matched against `self` and the move into `s` went
// unseen: the report lost its moved variant. The implicit-`this` spelling
// reported it all along.
struct Sink {
  void take(this Sink self, string &&s);
  void takeImplicit(string &&s);
};

string_view moved_through_explicit_object() {
  string local;
  string_view v = local;         // expected-warning {{may be returned later}}
  Sink{}.take(std::move(local)); // expected-note {{potentially moved here}}
  return v;                      // expected-note {{returned here}}
}

string_view moved_through_implicit_object() {
  string local;
  string_view v = local;                 // expected-warning {{may be returned later}}
  Sink{}.takeImplicit(std::move(local)); // expected-note {{potentially moved here}}
  return v;                              // expected-note {{returned here}}
}
