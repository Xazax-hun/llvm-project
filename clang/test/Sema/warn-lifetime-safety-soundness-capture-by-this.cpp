// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A [[clang::lifetime_capture_by(this)]] setter stashes the argument into the
// object. When the capture, the captured local's destruction, and a later read
// of the object all happen inside one method (the object outlives the local),
// the captured borrow dangles. The capture is modeled as a flow into the
// whole-object `this` origin (we do not know which member holds it); keeping
// `this` live at function exit lets the expiry check catch the captured local
// going out of scope while still held.

struct Latch {
  string_view last;
  void set(string_view sv [[clang::lifetime_capture_by(this)]]) { last = sv; }

  void capture_then_dangle() {
    {
      string tmp = "a long heap string value exceeding the sso buffer now!!";
      set(tmp); // expected-warning {{does not live long enough}}
    } // expected-note {{destroyed here}}
    const char *p = last.data(); // expected-note {{later used here}}
    (void)p;
  }
};

// Negative: the captured local outlives the read, so no dangle.
void captured_outlives_use() {
  string keep = "a long heap string value exceeding the sso buffer now!!";
  Latch l;
  l.set(keep);
  const char *p = l.last.data(); // no-warning
  (void)p;
}

//===----------------------------------------------------------------------===//
// A capture destination has to name the object that will hold the borrow. An
// inherited method is called on the derived object through an implicit
// derived-to-base conversion, whose own origin is a fresh node that merely COPIES
// the object's loans -- fine for reading through the upcast, useless for a write,
// so the captured borrow landed in the copy and the object never received it. The
// identical call on a `Base` object was reported, so the conversion alone decided
// whether the capture was modelled.
//
// The capture is routed by the loans the capturer's lvalue holds, and the upcast's
// lvalue carries the derived object's loan -- so this needs no special handling
// for the conversion, and the same routing covers any other spelling of the
// receiver.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] CapBase {
  string_view sv;
  void set(string_view s [[clang::lifetime_capture_by(this)]]) { sv = s; }
};

struct [[gsl::Pointer]] CapDerived : CapBase {};

volatile char csink;

// The control: a Base receiver needs no conversion, and was always reported.
void capture_into_base() {
  CapBase b{};
  {
    string t;
    b.set(t); // expected-warning {{local variable 't' does not live long enough}}
  }            // expected-note {{destroyed here}}
  csink = b.sv.data()[0]; // expected-note {{later used here}}
}

// The same call on a derived object, reached through the implicit conversion.
void capture_into_derived_through_base() {
  CapDerived d{};
  {
    string t;
    d.set(t); // expected-warning {{local variable 't' does not live long enough}}
  }            // expected-note {{destroyed here}}
  csink = d.sv.data()[0]; // expected-note {{later used here}}
}

// A captured borrow that OUTLIVES the object is not a dangle.
void capture_outlives_object() {
  string keep;
  {
    CapDerived d{};
    d.set(keep); // no-warning
    csink = d.sv.data()[0];
  }
}
