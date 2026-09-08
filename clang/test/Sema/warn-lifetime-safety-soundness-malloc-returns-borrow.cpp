// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;

// `__attribute__((malloc))` promises the returned pointer does not alias any object
// that already exists, so a call to such a function is modelled as a fresh
// allocation and the parameter-to-return propagation is never emitted. That makes
// the attribute a way to switch the check off: a function that actually returns a
// borrow of its parameter is silent with it and reported without it.
//
// Verify the body, the way the [[clang::lifetime_immortal]] promise is verified. A
// returned borrow has to be a fresh allocation; a borrow of a parameter, the
// implicit object, a local, or a global is a lie. This covers the annotated and the
// unannotated spelling alike, which a ban on combining `malloc` with
// [[clang::lifetimebound]] would not -- the early return does not depend on
// lifetimebound being present.

volatile char sink;

//===----------------------------------------------------------------------===//
// Reported: the returned pointer aliases something that already existed.
//===----------------------------------------------------------------------===//

// The reported shape: malloc plus lifetimebound, which was completely silent.
__attribute__((malloc)) const char *
grab_bound(const string &s [[clang::lifetimebound]]) { // expected-warning@-1 {{'__attribute__((malloc))' function returns a borrow of a parameter}}
  return s.c_str();
}

// The same lie with one annotation fewer. A ban on the combination would leave this
// to the unannotated-parameter demand -- whose suggestion is to add lifetimebound,
// i.e. to write the case above.
__attribute__((malloc)) const char *
grab_unbound(const string &s) { // expected-warning@-1 {{'__attribute__((malloc))' function returns a borrow of a parameter}}
  // expected-warning@-1 {{parameter that can hold a borrow is not annotated for lifetime safety}}
  // expected-warning@-2 {{should be marked [[clang::lifetimebound]]}}
  return s.c_str(); // expected-note {{param returned here}}
}

// A global is not a fresh allocation either.
static char g_buf[8];

__attribute__((malloc)) char *from_global() { // expected-warning {{'__attribute__((malloc))' function returns a borrow of a global or static}}
  return g_buf;
}

//===----------------------------------------------------------------------===//
// Must stay silent: the result really is fresh.
//===----------------------------------------------------------------------===//

__attribute__((malloc)) char *fresh(unsigned n) { // no-warning
  return new char[n];
}

// Returning immortal storage does alias something pre-existing, but it outlives
// every caller, so nothing can dangle through it.
[[clang::lifetime_immortal]] const char *immortal_str();

__attribute__((malloc)) const char *fresh_or_immortal() { // no-warning
  return immortal_str();
}

// Without the attribute the ordinary lifetimebound machinery applies and this is
// a perfectly good accessor.
const char *plain(const string &s [[clang::lifetimebound]]) { // no-warning
  return s.c_str();
}
