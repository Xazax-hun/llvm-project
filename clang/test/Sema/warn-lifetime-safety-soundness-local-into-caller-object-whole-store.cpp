// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

// A local of this call stored into an object the CALLER owns always dangles: the
// destination outlives the call and the local does not. No liveness question is
// needed, and none is available -- the read is in the caller, so nothing here keeps
// the borrow live and expiry never fires.
//
// That was checked from a FieldStore, which needs a MemberExpr to name the field, so
// only a store into a NAMED member was seen. A WHOLE-OBJECT store names none, and
// `dst = Box::wrap(buf)` went silent while `dst.p = buf` was reported.
//
// It is a backstop, so it yields to the expiry path: when the caller-side read
// happens to be in this function too, expiry reports it and points at the dangling
// use, which is the better diagnostic.

volatile char sink;

struct [[gsl::Owner(char)]] Box {
  static Box wrap(char *s [[clang::lifetimebound]]) {
    Box b;
    b.p = s;
    return b;
  }
  friend void via_reference(Box &dst [[clang::noescape]]);
  friend void via_pointer(Box *dst [[clang::noescape]]);
  friend void via_member(Box &dst [[clang::noescape]]);
  friend void into_local();
  friend void from_static(Box &dst [[clang::noescape]]);

private:
  char *p = nullptr; // expected-note {{this field dangles}}
};

//===----------------------------------------------------------------------===//
// Reported: a local parked in the caller's object.
//===----------------------------------------------------------------------===//

// The reported shape.
void via_reference(Box &dst [[clang::noescape]]) {
  char buf[8] = "hello";
  dst = Box::wrap(buf); // expected-warning {{stack memory associated with 'buf' escapes into an object the caller owns}}
}

// The pointer spelling was equally silent.
void via_pointer(Box *dst [[clang::noescape]]) {
  char buf[8] = "hello";
  *dst = Box::wrap(buf); // expected-warning {{stack memory associated with 'buf' escapes into an object the caller owns}}
}

// Into a NAMED member, which was reported all along -- and keeps its own wording,
// since it can name the field.
void via_member(Box &dst [[clang::noescape]]) {
  char buf[8] = "hello";
  dst.p = buf; // expected-warning {{stack memory associated with local variable 'buf' escapes to the field 'p' which will dangle}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A LOCAL destination is this call's own storage; the local's own expiry checks it.
void into_local() {
  Box b;
  {
    char buf[8] = "hello";
    b = Box::wrap(buf);
  }
  sink = 0; // no-warning: nothing reads the borrow
}

// A static outlives the call and is no hazard.
static char g_buf[8] = "hello";

void from_static(Box &dst [[clang::noescape]]) {
  dst = Box::wrap(g_buf); // no-warning
}
