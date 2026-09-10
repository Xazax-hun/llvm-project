// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused \
// RUN:   -Wno-dangling-gsl -Wno-lifetime-safety-lost-loan -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A default member initializer is code owned by a declaration. It is normally carried
// by whichever constructor runs it, and analyzed there -- but a class that is only
// ever AGGREGATE-initialized has no constructor at all, so the initializer's code
// appeared in no CFG anywhere and a hazard written in it was invisible. Adding
// `A() = default;` to the very same class made it report.
//
// A default member initializer is now analyzed on its own, the way a namespace-scope
// initializer and a default argument already are. Its `this` is the object under
// construction, identified by the field rather than by a method -- which is why the
// `this` placeholder had to stop insisting on a CXXMethodDecl.
//
// Analyzed only when no constructor that RUNS the initializer is defined in the
// translation unit; a copy or move constructor copies the member instead of running
// it, so it does not count. Otherwise the constructor reports and this would say the
// same thing twice.

volatile char sink;
static string g_mut = "a mutable global long enough to never be SSO at all";

//===----------------------------------------------------------------------===//
// Aggregate-only classes: the initializer is the only place the code appears.
//===----------------------------------------------------------------------===//

// The reported shape: bound to a sibling member.
struct [[gsl::Pointer(char)]] Sibling {
  string text;
  // expected-warning@+1 {{member is bound to a sibling member of the same object}}
  string_view v = text;
};

void useSibling() {
  Sibling a{"a heap string long enough to never be SSO at all"};
  sink = a.v.data()[0];
}

// Bound to a TEMPORARY, which dies at the end of the initializer. This is the case a
// per-class self-referential check alone would not have covered.
struct [[gsl::Pointer(char)]] Temp {
  // expected-warning@+1 {{local temporary object does not live long enough}}
  string_view v = string("a temporary long enough to never be SSO at all");
  // expected-note@-1 {{destroyed here}}
};

void useTemp() {
  Temp a{};
  sink = a.v.data()[0]; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Reported once, not twice, when a constructor carries the initializer.
//===----------------------------------------------------------------------===//

// A defaulted default constructor runs the initializer, so the constructor reports
// and the standalone analysis stands down.
struct [[gsl::Pointer(char)]] WithDefaultCtor {
  string text;
  string_view v = text;
  // The ctor path anchors at the CONSTRUCTOR; the standalone path anchors at the
  // initializer. Either way it is reported exactly once.
  // expected-warning@+1 {{member is bound to a sibling member of the same object}}
  WithDefaultCtor() = default;
};

void useWithDefaultCtor() {
  WithDefaultCtor b; // expected-note {{in defaulted default constructor for 'WithDefaultCtor' first required here}}
  sink = b.v.data()[0];
}

// A user constructor that does not initialize the member runs the initializer too.
struct [[gsl::Pointer(char)]] WithUserCtor {
  string text;
  string_view v = text;
  // expected-warning@+1 {{member is bound to a sibling member of the same object}}
  explicit WithUserCtor(int) {}
};

void useWithUserCtor() {
  WithUserCtor c{0};
  sink = c.v.data()[0];
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// An initializer that borrows nothing dangerous.
struct [[gsl::Pointer(char)]] FromGlobal {
  // (This one is loan-based at the USE, so it was already reported for aggregates
  // and is anchored in useQuiet below rather than here.)
  string_view v = g_mut;
};

// No borrow at all in the initializer.
struct Counter {
  int n = 0; // no-warning
};

void useQuiet() {
  FromGlobal f{};
  Counter c{};
  // expected-warning@+1 {{borrows from a mutable global or static object}}
  sink = (char)(c.n + f.v.data()[0]);
}
