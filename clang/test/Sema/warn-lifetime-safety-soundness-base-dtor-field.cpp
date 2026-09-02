// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A field-escape fact is deliberately suppressed inside a destructor: by the
// time one returns the object is gone, so "this borrow escapes to a field" is
// vacuous, and emitting it made a destructor's own cleanup look like it stranded
// a borrow.
//
// That reasoning does not hold for an INHERITED field. A base subobject outlives
// the derived destructor's BODY -- its destructor runs after the body returns and
// can read the base's own members -- so a borrow stored into an inherited member
// during the body is still held when ~Base runs. The CFG marks that point with a
// base-destructor element, and nothing consumed it, so the hazard was lost twice
// over: no field escape, and no use at the base's destruction either.
//
// The base's fields carry origins even though a base subobject has no origin of
// its own, so the base-destructor element now uses those: the body-local's expiry
// then has a live borrow and is reported.

volatile char sink;

struct Base {
  string_view v; // expected-note {{this field dangles}}
  ~Base() { sink = v.data()[0]; }
};

// The reported shape.
struct [[gsl::Pointer]] Derived : Base {
  ~Derived();
};
Derived::~Derived() {
  string local = "a long heap string value exceeding the sso buffer now";
  v = local; // expected-warning {{local variable 'local' does not live long enough}}
} // expected-note {{destroyed here}}
// expected-note@-1 {{later used here}}

// Every base gets its own destructor element, so each inherited field counts.
struct B1 {
  string_view a;
  ~B1() { sink = a.data()[0]; }
};
struct B2 {
  string_view b;
  ~B2() { sink = b.data()[0]; }
};

struct [[gsl::Pointer]] MultiBase : B1, B2 {
  ~MultiBase();
};
MultiBase::~MultiBase() {
  string local = "a long heap string value exceeding the sso buffer now";
  a = local; // expected-warning {{local variable 'local' does not live long enough}}
  b = local; // expected-warning {{local variable 'local' does not live long enough}}
} // expected-note 2 {{destroyed here}}
// expected-note@-1 2 {{later used here}}

// A field of a base OF a base is reached too.
struct Mid : B1 {};
struct [[gsl::Pointer]] Grandchild : Mid {
  ~Grandchild();
};
Grandchild::~Grandchild() {
  string local = "a long heap string value exceeding the sso buffer now";
  a = local; // expected-warning {{local variable 'local' does not live long enough}}
} // expected-note {{destroyed here}}
// expected-note@-1 {{later used here}}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A borrow that outlives the object is not a hazard, even in an inherited field.
struct [[gsl::Pointer]] StoresImmortal : Base {
  ~StoresImmortal();
};
StoresImmortal::~StoresImmortal() {
  static string keep = "a long heap string value exceeding the sso buffer";
  v = keep; // no-warning
}

// Nothing stored at all.
struct [[gsl::Pointer]] Empty : Base {
  ~Empty();
};
Empty::~Empty() {} // no-warning

// The destructor's OWN field is not read by anything after the body, so the
// suppression that made this quiet is still right -- only the inherited case
// needed changing.
struct [[gsl::Pointer]] OwnFieldOnly {
  string_view w;
  ~OwnFieldOnly();
};
OwnFieldOnly::~OwnFieldOnly() {
  string local = "a long heap string value exceeding the sso buffer now";
  w = local; // no-warning
}

// Storing into an inherited field OUTSIDE a destructor was always reported by
// the ordinary field-escape path; it must keep exactly one diagnostic.
struct [[gsl::Pointer]] NormalMethod : Base {
  void set();
};
void NormalMethod::set() {
  string local = "a long heap string value exceeding the sso buffer now";
  v = local; // expected-warning {{escapes to the field}}
}
