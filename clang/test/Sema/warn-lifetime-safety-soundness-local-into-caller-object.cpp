// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A LOCAL's borrow stored into a member of caller-owned storage dangles as soon
// as the function returns.
//
// The noescape check only fires when the SOURCE is an annotated parameter, so
// `c.d = s` with a noescape `s` was reported while `c.d = l.c_str()` with a local
// `l` was not. Nothing else covered it either:
//
//  - Expiry cannot see it. A [[gsl::Owner]]'s members are opaque, so a private
//    member has no origin of its own and the borrow lands on a transient
//    expression origin; at the local's expiry no live origin holds it.
//  - The multi-level-indirection refusal, which rejects a pointer-like
//    out-parameter and covered the [[gsl::Pointer]] spelling of this, does not
//    apply -- an owner is a single level of indirection.
//
// No liveness question arises: the destination outlives the call by definition
// and the local does not.

volatile char sink;

struct [[gsl::Owner(char)]] Box {
  friend void from_param(Box &c [[clang::noescape]],
                         const char *s [[clang::noescape]]);
  friend void from_local(Box &c [[clang::noescape]]);
  friend void from_temporary(Box &c [[clang::noescape]]);
  friend void by_value(Box c);
  friend void from_static(Box &c [[clang::noescape]]);
  friend void into_local_object();
  char peek() const { return d[0]; }

private:
  const char *d = ""; // expected-note {{this field dangles}}
};

// Reported all along: the source is an annotated parameter.
void from_param(Box &c [[clang::noescape]],
                const char *s [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  c.d = s; // expected-note {{escapes into an object the caller owns here}}
}

// Was silent: the source is a local, so no annotation is being broken -- the
// local simply does not outlive the caller's object.
void from_local(Box &c [[clang::noescape]]) {
  string l = "a long heap string value exceeding the sso buffer now";
  c.d = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' escapes to the field 'd' which will dangle}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A BY-VALUE parameter is the callee's own copy; writing it reaches nothing the
// caller holds.
void by_value(Box c) { // expected-warning {{parameter that can hold a borrow is not annotated}}
  string l = "a long heap string value exceeding the sso buffer now";
  c.d = l.c_str(); // no-warning
}

// A static outlives the call.
void from_static(Box &c [[clang::noescape]]) {
  static string keep = "a long heap string value exceeding the sso buffer";
  c.d = keep.c_str(); // expected-warning {{borrows from a mutable global or static object}}
}

// Storing into a LOCAL object is no escape -- the local's own expiry checks it.
void into_local_object() {
  Box b;
  string l = "a long heap string value exceeding the sso buffer now";
  b.d = l.c_str(); // no-warning
  sink = b.peek();
}
