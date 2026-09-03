// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// On a CONSTRUCTOR, '[[clang::lifetimebound]]' describes the CONSTRUCTED OBJECT,
// not a return value -- that is exactly what the ctor-capture ban tells authors
// to write instead of lifetime_capture_by(this). So the borrow coming to rest in
// the object is the declared relationship, and it is what VERIFIES the
// annotation.
//
// Both halves used to get this wrong, and the model contradicted its own advice:
// the capture was reported as undeclared, and the annotation was reported as
// unverifiable because no RETURN escape was ever seen -- on a constructor, which
// has no return value. The suggestion path already knew the rule ("suggest
// lifetimebound for a parameter escaping through a field in a constructor"); the
// verification and the undeclared-capture check now agree with it.

volatile char sink;

//===----------------------------------------------------------------------===//
// Truthful constructors: silent.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] InitList {
  string_view v;
  explicit InitList(string_view s [[clang::lifetimebound]]) : v(s) {} // no-warning
};

struct [[gsl::Pointer]] BodyStore {
  string_view v;
  explicit BodyStore(string_view s [[clang::lifetimebound]]) { v = s; } // no-warning
};

struct [[gsl::Pointer]] TwoParams {
  string_view a, b;
  TwoParams(string_view x [[clang::lifetimebound]],
            string_view y [[clang::lifetimebound]])
      : a(x), b(y) {} // no-warning
};

//===----------------------------------------------------------------------===//
// Still reported.
//===----------------------------------------------------------------------===//

// A lifetimebound constructor parameter the object never refers to: the
// annotation is unverifiable, and the message says "constructed object" rather
// than "return value" -- a constructor has none, and that wording is what made
// the original report unreadable.
struct [[gsl::Pointer]] Ignored {
  string_view v;
  explicit Ignored(string_view s [[clang::lifetimebound]]) { // expected-warning {{could not verify that the constructed object can be lifetime bound to 's'}}
    v = "an immortal string literal";
  }
};

// One parameter stored, one ignored: only the ignored one is flagged.
struct [[gsl::Pointer]] Partial {
  string_view a;
  Partial(string_view x [[clang::lifetimebound]],
          string_view y [[clang::lifetimebound]]) // expected-warning {{could not verify that the constructed object can be lifetime bound to 'y'}}
      : a(x) {}
};

// A non-constructor keeps the return-value reading, and a member store there is
// still an undeclared capture.
struct [[gsl::Pointer]] Method {
  string_view v;
  string_view take(string_view p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE}}
    v = p;
    return p;
  }
  string_view unrelated(string_view p [[clang::lifetimebound]]) { // expected-warning {{could not verify that the return value can be lifetime bound to 'p'}}
    return v;
  }
};

// The dangling use itself is still caught: the object outlives the argument.
void real_dangle() {
  string_view out;
  {
    string t = "a long heap string value exceeding the sso buffer now";
    InitList a{t}; // expected-warning {{local variable 't' does not live long enough}}
    out = a.v;
  }                       // expected-note {{destroyed here}}
  sink = out.data()[0];   // expected-note {{later used here}}
}
