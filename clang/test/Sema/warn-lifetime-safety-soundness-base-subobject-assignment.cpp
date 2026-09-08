// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused \
// RUN:   -Wno-lifetime-safety-lost-loan -verify %s
//
// The lost-borrow sentinel is switched off here: it fires at every use of a derived
// [[gsl::Pointer]] in these functions, independently of this check (it did so before
// this fix too), and it is a refusal rather than the report under test.

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// Assigning to a BASE SUBOBJECT reaches the base and nothing else. But a
// [[gsl::Pointer]] object is a single origin, so the base subobject and the whole
// object are that one origin -- and the casts that name the base are stripped before
// the store is modelled (the derived-to-base reference cast by the value-preserving
// cast loop, the implicit one by IgnoreParenImpCasts). What arrived looked like a
// store covering the whole object, so it KILLED the object's loans and its liveness.
//
// That discarded borrows the store never touched, including one held by a member the
// derived class adds. A dangle in such a member went silent, while the same code
// without the base assignment reported it.
//
// Now asked by comparing the type as SPELLED at the destination against the lvalue
// left after stripping: equal for a whole-object store, narrower for a slice of one.
// A slice merges instead of killing, and does not mark the destination written.

volatile char sink;
static const char kImmortal[] = "immortal";

struct [[gsl::Pointer(char)]] Base {
  string_view b;
  Base() = default;
  explicit Base(string_view s [[clang::lifetimebound]]) : b(s) {}
};

struct [[gsl::Pointer(char)]] Derived : Base {
  string_view v; // the member the base assignment does not touch
  Derived() = default;
  void use() const { sink = v.data()[0]; }
};

//===----------------------------------------------------------------------===//
// The dangle in the derived member survives a reseat of the base.
//===----------------------------------------------------------------------===//

// The reported shape: an explicit cast to the base.
void reseat_base_via_cast() {
  Derived d;
  d.v = string_view(kImmortal);
  {
    string tmp("temporary");
    d.v = tmp; // expected-warning {{local variable 'tmp' does not live long enough}}
    static_cast<Base &>(d) = Base(string_view(kImmortal));
  } // expected-note {{destroyed here}}
  d.use(); // expected-note {{later used here}}
}

// The same call spelled as a qualified member call.
void reseat_base_via_qualified_call() {
  Derived d;
  d.v = string_view(kImmortal);
  {
    string tmp("temporary");
    d.v = tmp; // expected-warning {{local variable 'tmp' does not live long enough}}
    d.Base::operator=(Base(string_view(kImmortal)));
  } // expected-note {{destroyed here}}
  d.use(); // expected-note {{later used here}}
}

// Control: no base assignment at all -- reported all along, and the three must agree.
void no_reseat() {
  Derived d;
  d.v = string_view(kImmortal);
  {
    string tmp("temporary");
    d.v = tmp; // expected-warning {{local variable 'tmp' does not live long enough}}
  } // expected-note {{destroyed here}}
  d.use(); // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must keep behaving as whole-object stores.
//===----------------------------------------------------------------------===//

// A WHOLE-object assignment does overwrite everything, so it still kills: the
// borrow of `tmp` is gone by the time the object is read.
void whole_object_assignment() {
  Derived d;
  {
    string tmp("temporary");
    d.v = tmp;
    d = Derived(); // no-warning: this really does overwrite `v`
  }
  d.use();
}

// The reference cast this stripping loop exists for preserves the referent type, so
// it is a full store and must not become a merge.
void value_preserving_cast() {
  const char *p = kImmortal;
  const char *q = kImmortal;
  static_cast<const char *&>(p) = q; // no-warning
  sink = *p;
}

// An array subscript destination leaves the analysis with no type for the lvalue.
// Reading one crashed the frontend, which looked exactly like "no diagnostics".
struct [[gsl::Pointer(char)]] Holder {
  string_view v;
};

void array_element_destination() {
  Holder a[2];
  a[0].v = string_view(kImmortal); // no-warning
  sink = a[0].v.data()[0];
}
