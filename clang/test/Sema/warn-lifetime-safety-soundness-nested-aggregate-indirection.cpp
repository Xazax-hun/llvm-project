// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::vector;
using std::string_view;

// A plain (non-template) aggregate whose member is -- or transitively contains
// -- an owner-/pointer-of-indirection (e.g. std::vector<std::string_view>) is
// just as untrackable as the wrapped forms the analysis already rejects, but
// findNestedOwnerOrPointerOfIndirection only descended template arguments, not
// plain record member fields. A local/parameter/return/member of such a type
// therefore slipped (the only backstop was the field-level flag at the inner
// record's own definition, which is suppressed in system headers). The search
// now descends plain record members too.

struct Inner {
  vector<string_view> v; // expected-warning {{is a container whose element type holds a borrow}}
};

// A plain wrapper of the inner record: rejected at its member declaration...
struct Outer {
  Inner inner; // expected-warning {{is a container whose element type holds a borrow}}
};

// ...and at a local/return of the wrapper type.
Outer make_outer() {
  Outer o; // expected-warning {{is a container whose element type holds a borrow}}
  return o;
}

// Nested two levels deep through plain aggregates.
struct Mid {
  Inner mid; // expected-warning {{is a container whose element type holds a borrow}}
};
struct Outer2 {
  Mid m; // expected-warning {{is a container whose element type holds a borrow}}
};
void local_nested() {
  Outer2 o; // expected-warning {{is a container whose element type holds a borrow}}
  (void)o;
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A plain aggregate with no indirection-holding member is fine.
struct PlainVal {
  int a;
  double b;
};
struct WrapVal {
  PlainVal v;
};
WrapVal make_plain() { return WrapVal{}; } // no-warning
