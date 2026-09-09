// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused \
// RUN:   -Wno-dangling -Wno-dangling-gsl -Wno-dangling-assignment-gsl \
// RUN:   -Wno-lifetime-safety-lost-loan -verify %s
//
// The lost-borrow sentinel is switched off: a value that holds no borrow draws it
// throughout here, and it is not what is under test. (The refusal added for this
// case lives in the unannotated-indirection group, so that one stays on.)

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A default argument's expression belongs to the CALLEE's declaration, not to the
// caller, so the CFG deliberately does not contain it -- adding it would make one
// Expr appear at every call site (PR13385, the FIXME in CFG.cpp). The analysis
// therefore never sees a temporary the default argument materializes: no loan is
// issued for it and no expiry fires, so the borrow it hands over looks immortal, and
// `h = Holder();` was silent where the identical `h = Holder(string(...))` is
// reported precisely.
//
// Modelling it in the generator instead would share one expression's origins across
// every call site, which is the aliasing hazard the CFG comment is about. So the call
// is refused rather than mis-modelled.
//
// Narrow on both counts, because default arguments are ordinary: only when the
// argument materializes a TEMPORARY, and only when the parameter's annotation lets
// the borrow outlive the call.

volatile char sink;
static const char kImmortal[] = "immortal";

//===----------------------------------------------------------------------===//
// Refused: a temporary bound to a parameter whose borrow can outlive the call.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Holder {
  string_view v;
  Holder(string_view s [[clang::lifetimebound]] = string("default")) : v(s) {}
  char first() const { return v.data()[0]; }
};

void via_constructor() {
  // expected-warning@+1 {{a default argument that creates a temporary is not modeled}}
  Holder h{};
  sink = h.first();
}

// A free function, which reaches the refusal by a different path.
string_view pick(string_view s [[clang::lifetimebound]] = string("default")) {
  return s;
}

void via_function() {
  // expected-warning@+1 {{a default argument that creates a temporary is not modeled}}
  string_view r = pick();
  sink = r.data()[0];
}

// `lifetime_capture_by` lets it outlive the call just as much.
struct [[gsl::Pointer(char)]] Sink {
  string_view v;
  void take(string_view s [[clang::lifetime_capture_by(this)]] = string("default")) {
    v = s;
  }
};

// (A gsl::Pointer out-parameter is two levels, so it also draws the
// multilevel-indirection refusal; unrelated to this check.)
void via_capture_by(Sink &s [[clang::noescape]]) { // expected-warning {{uses more than one level of indirection}}
  s.take(); // expected-warning {{a default argument that creates a temporary is not modeled}}
}

//===----------------------------------------------------------------------===//
// The explicit spelling is modelled precisely and must stay that way.
//===----------------------------------------------------------------------===//

void explicit_temporary() {
  Holder h{kImmortal};
  h = Holder(string("default")); // expected-warning {{local temporary object does not live long enough}}
  // expected-note@-1 {{destroyed here}}
  sink = h.first(); // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: nothing dies, or the borrow cannot outlive the call.
//===----------------------------------------------------------------------===//

// A literal default argument materializes no temporary.
string_view from_literal(string_view s [[clang::lifetimebound]] = kImmortal) {
  return s;
}

// Neither does a null pointer.
const char *from_null(const char *s [[clang::lifetimebound]] = nullptr) { return s; }

// A temporary bound to a parameter the borrow cannot outlive.
void noescape_param(string_view s [[clang::noescape]] = string("default")) {
  sink = s.data()[0];
}

void quiet() {
  (void)from_literal(); // no-warning
  (void)from_null();    // no-warning
  noescape_param();     // no-warning
}
