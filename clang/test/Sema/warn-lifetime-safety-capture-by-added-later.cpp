// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused \
// RUN:   -Wno-lifetime-safety-unannotated-indirection -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string_view;

// A call is checked against the STATICALLY resolved callee, so an override that adds
// a lifetime contract the base does not declare is invisible to every caller
// dispatching through the base. That is why adding '[[clang::lifetimebound]]' in an
// override is already refused.
//
// Adding '[[clang::lifetime_capture_by]]' is the same hole and a worse one: the base
// declares that the callee merely borrows for the duration of the call, so a caller
// hands over an argument it must not let escape -- and the override parks it in the
// object. Nothing is reported at the call site, because it is checked against the
// base; and nothing is reported in the override either, because its body does
// exactly what its own annotation permits.

volatile char sink;

//===----------------------------------------------------------------------===//
// Refused: the override adds a capture the base does not declare.
//===----------------------------------------------------------------------===//

struct Observer {
  virtual ~Observer() = default;
  virtual void onEvent(string_view payload) {} // expected-note 2 {{overridden virtual function is here}}
};

// The reported shape: capture into the object.
struct [[gsl::Pointer(char)]] CachingObserver : Observer {
  string_view last;
  // expected-warning@+1 {{overriding parameter 'payload' adds '[[clang::lifetime_capture_by]]' not present on the overridden method}}
  void onEvent(string_view payload [[clang::lifetime_capture_by(this)]]) override {
    last = payload;
  }
};

// Naming another PARAMETER rather than `this` is the same statement about the call.
struct Forwarder : Observer {
  // expected-warning@+1 {{overriding parameter 'payload' adds '[[clang::lifetime_capture_by]]' not present on the overridden method}}
  void onEvent(string_view payload [[clang::lifetime_capture_by(this)]]) override {
    sink = payload.data()[0];
  }
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// The base already declares the capture, so the override repeating it adds nothing
// and callers through the base are checked against it.
struct [[gsl::Pointer(char)]] CapturingBase {
  string_view last;
  virtual ~CapturingBase() = default;
  virtual void onEvent(string_view payload [[clang::lifetime_capture_by(this)]]) {
    last = payload;
  }
};

struct [[gsl::Pointer(char)]] Repeats : CapturingBase {
  void onEvent(string_view payload [[clang::lifetime_capture_by(this)]]) override { // no-warning
    last = payload;
  }
};

// DROPPING the capture is safe: callers through the base already assume it happens,
// which is the conservative direction.
struct [[gsl::Pointer(char)]] Drops : CapturingBase {
  void onEvent(string_view payload) override { // no-warning
    sink = payload.data()[0];
  }
};

//===----------------------------------------------------------------------===//
// The same hole without a virtual: a later DECLARATION adding the capture.
//===----------------------------------------------------------------------===//

// A call is checked against the declaration in scope, so an attribute added on the
// out-of-line definition is invisible to every call that precedes it -- and the
// definition's body does exactly what its own annotation permits, so neither end
// reports. C++ has the same rule for carries_dependency: the first declaration has
// to specify it.
struct [[gsl::Pointer(char)]] Split {
  string_view held;
  void pick(string_view a); // expected-note {{first declaration of the parameter is here}}
};

// expected-warning@+1 {{'[[clang::lifetime_capture_by]]' on parameter 'a' is missing from the first declaration of this function}}
void Split::pick(string_view a [[clang::lifetime_capture_by(this)]]) { held = a; }

// Written on the in-class declaration and not repeated: propagated to the
// definition, so the definition's parameter is annotated too and its body is checked
// against the promise rather than demanding an annotation of its own.
struct [[gsl::Pointer(char)]] SplitHonest {
  string_view held;
  void pick(string_view a [[clang::lifetime_capture_by(this)]]);
};

void SplitHonest::pick(string_view a) { held = a; } // no-warning

// Written on both is fine.
struct [[gsl::Pointer(char)]] SplitRepeats {
  string_view held;
  void pick(string_view a [[clang::lifetime_capture_by(this)]]);
};

void SplitRepeats::pick(string_view a [[clang::lifetime_capture_by(this)]]) { // no-warning
  held = a;
}
