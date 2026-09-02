// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A capture destination has to name the object that will hold the borrow. When
// the receiver is a MEMBER, the capture was flowing into the origin of the
// r-value that reading the member produced -- a throwaway that the next read of
// the same member does not share -- so the borrow never reached the object.
//
// It reads as clean rather than as a lost borrow because the enclosing object
// already carries an unrelated loan: the lost-loan sentinel only fires when NO
// borrow information reaches an origin, so a co-resident loan hides the drop.
// `member_capture_no_prior_loan` below is the same bug without that masking.
//
// The store is routed by the loans the receiver's lvalue holds. Those name the
// storage written -- `h.v` -- and resolving that path walks to the origin a
// later read of `h.v` consults, so the deposit and the read meet on the same
// node whatever expression designated the member.

volatile char sink;

struct [[gsl::Pointer]] View {
  string_view sv;
  void set(string_view s [[clang::lifetime_capture_by(this)]]) { sv = s; }
};

struct [[gsl::Pointer]] Holder { View v; };
struct [[gsl::Pointer]] Nested { Holder h; };

// A gsl::Pointer record is a single origin for the whole object, so `h.v` has
// no origin of its own and the store must land on the enclosing object -- which
// is exactly what the read consults.
void member_capture(string &keeper) {
  Holder h{View{keeper}}; // the prior loan that masked the drop
  {
    string local = "a long heap string value exceeding the sso buffer now!!";
    h.v.set(local); // expected-warning {{local variable 'local' does not live long enough}}
  }                 // expected-note {{destroyed here}}
  sink = h.v.sv.data()[0]; // expected-note {{later used here}}
}

// The same bug with nothing to mask it: previously reported only as a lost
// borrow, now reported precisely.
void member_capture_no_prior_loan() {
  Holder h{};
  {
    string local = "a long heap string value exceeding the sso buffer now!!";
    h.v.set(local); // expected-warning {{local variable 'local' does not live long enough}}
  }                 // expected-note {{destroyed here}}
  sink = h.v.sv.data()[0]; // expected-note {{later used here}}
}

// Two member steps.
void nested_member_capture() {
  Nested n{};
  {
    string local = "a long heap string value exceeding the sso buffer now!!";
    n.h.v.set(local); // expected-warning {{local variable 'local' does not live long enough}}
  }                   // expected-note {{destroyed here}}
  sink = n.h.v.sv.data()[0]; // expected-note {{later used here}}
}

// Reached through a reference to the member rather than the member expression.
void member_capture_through_ref() {
  Holder h{};
  View &r = h.v;
  {
    string local = "a long heap string value exceeding the sso buffer now!!";
    r.set(local); // expected-warning {{local variable 'local' does not live long enough}}
  }               // expected-note {{destroyed here}}
  sink = h.v.sv.data()[0]; // expected-note {{later used here}}
}

// A plain record DOES give its field an origin of its own, so the path resolves
// one step further. Both spellings have to work.
struct PlainHolder { View v; };

void plain_member_capture() {
  PlainHolder h{};
  {
    string local = "a long heap string value exceeding the sso buffer now!!";
    h.v.set(local); // expected-warning {{local variable 'local' does not live long enough}}
  }                 // expected-note {{destroyed here}}
  sink = h.v.sv.data()[0]; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// The captured borrow outlives the object holding it.
void capture_outlives_holder(string &keeper) {
  {
    Holder h{};
    h.v.set(keeper); // no-warning
    sink = h.v.sv.data()[0];
  }
}

// Captured and read while the source is still alive.
void capture_still_alive() {
  Holder h{};
  string keep = "a long heap string value exceeding the sso buffer now!!";
  h.v.set(keep); // no-warning
  sink = h.v.sv.data()[0];
}

// A store into a SIBLING member is not a store into the one that is read. Both
// members of a gsl::Pointer record share one origin, so this is conservative
// rather than precise -- it must not go silent, but it also must not be
// reported as a dangle of something never captured.
struct [[gsl::Pointer]] TwoViews { View a; View b; };

void sibling_member(string &keeper) {
  TwoViews t{};
  t.a.set(keeper); // no-warning
  sink = t.b.sv.data()[0];
}
