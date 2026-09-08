// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A DELEGATING initializer (`: E(init)`) stores into the object being constructed --
// more directly than a base initializer does, since the delegated-to constructor
// initializes this very object rather than a subobject of it. Only a base was tested
// for, so a delegating one fell through and modelled nothing.
//
// A [[clang::noescape]] parameter forwarded to a delegated-to constructor that stores
// it therefore came to rest in the object with nothing said, while the same parameter
// stored directly by this constructor was reported. Callers trust noescape and drop
// the borrowed object, leaving the constructed one dangling.

volatile char sink;

//===----------------------------------------------------------------------===//
// Reported: the parameter comes to rest in the object.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] E {
  string_view v; // expected-note {{escapes to this field}}

  E(string_view s [[clang::lifetimebound]], int) : v(s) {}

  // The reported shape: forwarded to a constructor that stores it.
  E(string_view s [[clang::noescape]], char) // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
      : E(s, 0) {}                           // expected-note {{param returned here}}

  // The direct spelling, which was reported all along -- the two must agree.
  E(string_view s [[clang::noescape]], long) // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
      : v(s) {}

  void use() const { sink = v.data()[0]; }
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Quiet {
  string_view v;

  Quiet(string_view s [[clang::lifetimebound]], int) : v(s) {}

  // Delegating with a lifetimebound parameter is the honest spelling: on a
  // constructor the annotation describes the constructed object, and the borrow
  // coming to rest in it is exactly what verifies the annotation.
  Quiet(string_view s [[clang::lifetimebound]], char) : Quiet(s, 0) {} // no-warning

  // A noescape parameter the delegated-to constructor does not store. (Delegating
  // to a borrowless constructor flows nothing into the object, so the lost-borrow
  // sentinel fires on the initializer -- a refusal, not this check.)
  // expected-warning@+1 {{lifetime safety cannot track this value here}}
  Quiet(string_view s [[clang::noescape]], long) : Quiet(string_view(), 0) {
    sink = s.data()[0];
  }
};

// Delegating to a constructor that borrows nothing.
struct [[gsl::Pointer(char)]] Empty {
  string_view v;
  Empty() : v() {}
  Empty(string_view s [[clang::noescape]], int) : Empty() { sink = s.data()[0]; } // no-warning
};
