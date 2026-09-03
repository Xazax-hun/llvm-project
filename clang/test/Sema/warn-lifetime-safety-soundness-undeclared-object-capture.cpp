// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string_view;

namespace std {
using size_t = decltype(sizeof(0));
}
void *operator new(std::size_t, void *p) noexcept;

// '[[clang::lifetimebound]]' describes the RETURN VALUE and nothing else, so a
// body that ALSO captures the parameter into the object is making a second,
// undeclared promise: a caller reading the declaration keeps the argument alive
// for the result, not for the object, and discarding the result then leaves the
// object holding a dangling borrow.
//
// The check keyed on a store into a NAMED member, so only the direct `v = p`
// spelling was caught. Every way of landing the borrow on the OBJECT instead was
// silent: a whole-object assignment, a store through a named temporary, a
// placement new, a helper declared lifetime_capture_by(this), or an inherited
// setter. A borrow resting in the object at exit is now an escape in its own
// right, so all of them are reported.

volatile char sink;

// The direct spelling, caught all along.
struct [[gsl::Pointer]] Direct {
  string_view v;
  string_view take(string_view p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE}}
    v = p;
    return p;
  }
};

// Through a helper that declares the capture. The helper is honest; `take` is
// not.
struct [[gsl::Pointer]] ViaHelper {
  string_view v;
  void setD(string_view p [[clang::lifetime_capture_by(this)]]) { v = p; }
  string_view take(string_view p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE}}
    setD(p);
    return p;
  }
};

// Whole-object assignment.
struct [[gsl::Pointer]] WholeObject {
  string_view v;
  string_view take(string_view p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE}}
    *this = WholeObject{p};
    return p;
  }
};

// Through a named temporary.
struct [[gsl::Pointer]] ViaTemporary {
  string_view v;
  string_view take(string_view p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE}}
    ViaTemporary t{p};
    *this = t;
    return p;
  }
};

// Placement new.
struct [[gsl::Pointer]] ViaPlacement {
  string_view v;
  string_view take(string_view p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE}}
    new (this) ViaPlacement{p};
    return p;
  }
};

// An inherited setter, so the capture is not even written in this class.
struct [[gsl::Pointer]] CapBase {
  string_view b;
  void setB(string_view p [[clang::lifetime_capture_by(this)]]) { b = p; }
};

struct [[gsl::Pointer]] ViaInherited : CapBase {
  string_view take(string_view p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE}}
    setB(p);
    return p;
  }
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] Honest {
  string_view v;
  // Declares the capture, so there is nothing undeclared about it.
  void declared(string_view p [[clang::lifetime_capture_by(this)]]) { v = p; }
  // Returns the parameter and captures nothing.
  string_view justReturn(string_view p [[clang::lifetimebound]]) { return p; }
  // Touches no parameter at all.
  string_view w;
  void shuffle() { w = v; }
  void read() const { sink = v.data()[0]; }
};
