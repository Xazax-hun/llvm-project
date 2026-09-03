// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string_view;

namespace std {
using size_t = decltype(sizeof(0));
}
void *operator new(std::size_t, void *p) noexcept;

// A [[clang::noescape]] borrow stored through an lvalue that names CALLER-OWNED
// storage escapes, exactly as a store into a named field of `this` does.
//
// Only the named-member spelling was checked, so `*this = S{q}` -- which writes
// the whole object -- escaped unreported while `v = q` one line away was caught.
// The escape check at function exit does not cover it either: it inspects the
// FIELD origins, and a gsl::Pointer record is a single leaf origin with no field
// edges, so a borrow deposited on the object sits on no field's origin.
//
// The destination's loans have to be read from BEFORE the store. Afterwards the
// lvalue also holds the store's own payload, whose loans are parameter
// placeholders -- which would make the payload look like the destination.

struct [[gsl::Pointer]] S {
  string_view v; // expected-note {{escapes to this field}}

  // Reported all along: a store into a named member.
  void field_store(string_view q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    v = q;
  }

  // Was silent: the whole object is written instead of one member.
  void whole_object(string_view q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    *this = S{q}; // expected-note {{escapes into an object the caller owns here}}
  }

  // Through a named temporary, so the borrow reaches the object one step later.
  void via_temporary(string_view q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    S tmp{q};
    *this = tmp; // expected-note {{escapes into an object the caller owns here}}
  }

  // A placement new writes the object too.
  void placement(string_view q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    new (this) S{q}; // expected-note {{escapes into an object the caller owns here}}
  }
};

// The destination reached through a PARAMETER rather than `this`.
void through_param(S *out, string_view q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  // expected-warning@-1 {{parameter 'out' uses more than one level of indirection}}
  *out = S{q}; // expected-note {{escapes into an object the caller owns here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A whole-object store of something that is NOT the noescape parameter's borrow.
void unrelated(S *out, string_view q [[clang::noescape]]) { // expected-warning {{parameter 'out' uses more than one level of indirection}}
  *out = S{};
  (void)q; // no-warning
}

// Writing a LOCAL object is no escape.
volatile char sink;
void into_local(string_view q [[clang::noescape]]) {
  S local;
  local = S{q}; // no-warning
  sink = local.v.data()[0];
}

// The parameter is not noescape, so storing it is the caller's business.
void not_annotated(S *out, string_view q) { // expected-warning {{parameter 'out' uses more than one level of indirection}}
  // expected-warning@-1 {{parameter that can hold a borrow is not annotated}}
  *out = S{q};                              // no noescape violation
}
