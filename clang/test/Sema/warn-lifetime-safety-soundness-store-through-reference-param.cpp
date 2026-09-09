// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

// A whole-object assignment through a REFERENCE writes the caller's object exactly as
// `*ptr = ...` does. But the destination resolves statically, so only a flow was
// emitted and no store fact existed for the store-site checks to see -- while the
// pointer spelling goes through the routed path and does emit one.
//
// So `*dst = Box::wrap(s)` reported a [[clang::noescape]] argument coming to rest in
// the caller's object, and `dst = Box::wrap(s)` -- same annotations, same escape, only
// the destination's form differing -- said nothing. Storing into a MEMBER of the
// reference was reported too, which is what places this on the whole-object store
// rather than on references in general.

volatile char sink;

// An OWNER destination is a single level of indirection, so `Box &` draws no
// multilevel-indirection refusal and the escape has to be caught on its own.
struct [[gsl::Owner(char)]] Box {
  static Box wrap(char *s [[clang::lifetimebound]]) {
    Box b;
    b.p = s;
    return b;
  }
  // The attributes are part of the type, so the friend declarations must repeat
  // them or they declare different functions.
  friend void via_reference(Box &dst [[clang::noescape]], char *s [[clang::noescape]]);
  friend void via_pointer(Box *dst [[clang::noescape]], char *s [[clang::noescape]]);
  friend void via_member(Box &dst [[clang::noescape]], char *s [[clang::noescape]]);
  friend void via_local(char *s [[clang::noescape]]);

private:
  char *p = nullptr;
};

//===----------------------------------------------------------------------===//
// Every spelling of "the argument comes to rest in the caller's object".
//===----------------------------------------------------------------------===//

// The reported shape.
void via_reference(Box &dst [[clang::noescape]],
    char *s [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  dst = Box::wrap(s); // expected-note {{escapes into an object the caller owns here}}
}

// Through a pointer parameter -- reported all along.
void via_pointer(Box *dst [[clang::noescape]],
    char *s [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  *dst = Box::wrap(s); // expected-note {{escapes into an object the caller owns here}}
}

// Into a member of the reference -- also reported all along.
void via_member(Box &dst [[clang::noescape]],
    char *s [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  dst.p = s; // expected-note {{escapes into an object the caller owns here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: the destination does not outlive the call.
//===----------------------------------------------------------------------===//

// A LOCAL destination is this call's own storage, so nothing escapes.
void via_local(char *s [[clang::noescape]]) {
  Box b;
  b = Box::wrap(s); // no-warning
  sink = 0;
}
