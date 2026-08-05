// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// `this->arr[i]` denotes the array member `this->arr` -- all elements share the
// array's single origin, so a per-element store or mutation is a store/mutation
// of the member. The AST-shape-based checks (const-subversion and the
// self-referential field store) previously matched only a direct `this->member`
// and missed the array-element form `this->arr[i]`, so a const method that
// reallocates an owner through an array-of-pointers self-alias (and the
// self-aliasing store itself) went unflagged. The peel is now centralized in
// memberThroughArraySubscripts, used at every such site.

class [[gsl::Owner(char)]] MyStr {
  string buf{"x"};
  string *self[1]; // private array-of-pointers self alias
  // The self-aliasing borrow lives in this member, so each mutation through it is
  // also reported against the member (once per mutating method below).
  // expected-warning@-3 2 {{borrow held by this member which escapes to a field is later invalidated}}
  // expected-note@-4 2 {{this field dangles}}

public:
  MyStr() {
    self[0] = &buf; // expected-warning {{member is bound to a sibling member of the same object, making the object self-referential}}
  }
  string_view view() const [[clang::lifetimebound]] { return buf; }
  // A const method reallocating the owner through the array element pointer.
  void grow() const {
    self[0]->push_back('z'); // expected-warning {{mutating an owner through a pointer member}} expected-note {{invalidated here}}
  }
  // The `*this->arr[i]` deref form is covered too.
  void grow_deref() const {
    (*self[0]).push_back('z'); // expected-warning {{mutating an owner through a pointer member}} expected-note {{invalidated here}}
  }
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A subscript of a *pointer* member (not an array) is an ordinary indirection,
// not an array-element member access -- the existing direct-pointer handling
// still applies, the synthetic array peel does not misfire.
struct OkArrayValues {
  int data[4];
  int read() const { return data[0]; } // no-warning: not a pointer-to-owner
};
