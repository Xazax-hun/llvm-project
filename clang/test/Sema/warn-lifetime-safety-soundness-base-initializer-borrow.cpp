// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A base-class initializer stores into the base subobject of `this`, exactly as a
// member initializer stores into a member. The member path flows the initializer's
// origins into the member, so a temporary bound there is seen to die; the base path
// only emitted an escape fact -- which lets the annotation verifier see an escaped
// PARAMETER loan, but deposits nothing.
//
// So the borrow rested nowhere, and a temporary passed to a base constructor died
// unobserved: the destructor's read of it was missed, while the identical
// initializer of a MEMBER of the same type was reported.

volatile char g;

struct [[gsl::Pointer]] Logger {
  string_view label;
  explicit Logger(string_view l [[clang::lifetimebound]]) : label(l) {}
  ~Logger() { g = label.data()[0]; }
};

//===----------------------------------------------------------------------===//
// Caught: a temporary bound through an initializer.
//===----------------------------------------------------------------------===//

// The reported shape: through a BASE initializer.
struct [[gsl::Pointer]] ViaBase : Logger {
  ViaBase() : Logger(string("temporary")) {} // expected-warning {{local temporary object does not live long enough}}
  // expected-note@-1 {{destroyed here}} expected-note@-1 {{later used here}}
};

// Through a MEMBER initializer, which was reported all along -- the two must agree.
struct [[gsl::Pointer]] ViaMember {
  Logger m; // expected-note {{pointer member declared here}} expected-note {{this field dangles}}
  // expected-warning@+2 {{initializing pointer member 'm' to point to a temporary object}}
  // expected-warning@+1 {{stack memory associated with local temporary object escapes to the field 'm'}}
  ViaMember() : m(string("temporary")) {}
};

//===----------------------------------------------------------------------===//
// Must stay silent: the borrowed object outlives the constructed one.
//===----------------------------------------------------------------------===//

static const string kGlobal = "global";

struct [[gsl::Pointer]] ViaGlobal : Logger {
  ViaGlobal() : Logger(kGlobal) {} // no-warning
};

// A base initialized from a parameter the caller keeps alive. Depositing the borrow
// makes this verify: on a constructor [[clang::lifetimebound]] describes the
// CONSTRUCTED OBJECT, and the borrow coming to rest in the object is that escape. It
// only verifies because the deposit lands on the `this` origin -- the one a
// whole-object store resolves to and the one the exit escape reads. On the pointee it
// would still be live (the exit use covers both levels) but invisible here, and this
// reported "could not verify" before.
struct [[gsl::Pointer]] ViaParam : Logger {
  explicit ViaParam(const string &s [[clang::lifetimebound]]) : Logger(s) {} // no-warning
};
