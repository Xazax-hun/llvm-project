// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// `lifetime_capture_by(this)` promises the borrow comes to rest in the capturing
// object. A class with no borrow-holding member has no origin for it to rest in, so
// the capture was dropped at the call and no later dangling use could be connected
// to it.
//
// The realistic way to meet this is inheritance. A view out-parameter is refused as
// two levels of indirection, but that measures the STATIC type: a plain base
// measures one level, so a `Base &` parameter whose dynamic type is a
// [[gsl::Pointer]] slips past the refusal while a `Derived &` parameter does not.
// The borrow then lands in the caller's object with nothing said.
//
// Reported at the CALL rather than at the declaration, because that is where the
// capture is dropped -- and it therefore also covers a callee that is only declared
// in this TU, which a declaration-side check on the definition would miss.

volatile char sink;

//===----------------------------------------------------------------------===//
// Refused: the capturing object has nowhere to put the borrow.
//===----------------------------------------------------------------------===//

// No members at all, and only declared -- there is no body to analyse.
struct Empty {
  void set(string_view in [[clang::lifetime_capture_by(this)]]);
};

void via_empty(Empty &e [[clang::noescape]]) {
  string b("xxxx");
  e.set(b); // expected-warning {{the capturing object's type has no member the borrow could rest in}}
}

// The reported shape: a plain polymorphic base whose override holds the borrow.
struct Base {
  virtual ~Base() = default;
  virtual string_view fmt(string_view in [[clang::lifetimebound]]
                                         [[clang::lifetime_capture_by(this)]]) {
    return in;
  }
};

struct [[gsl::Pointer]] Caching : Base {
  string_view cache;
  string_view fmt(string_view in [[clang::lifetimebound]]
                                 [[clang::lifetime_capture_by(this)]]) override {
    cache = in;
    return in;
  }
};

void render(Base &f [[clang::noescape]]) {
  string b("xxxx");
  f.fmt(b); // expected-warning {{the capturing object's type has no member the borrow could rest in}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: the object can hold the borrow.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] Holder {
  string_view cache;
  void set(string_view in [[clang::lifetime_capture_by(this)]]) { cache = in; }
};

// Reaching a holder by its own type is two levels, so it draws the DEPTH refusal --
// never this one, because the capture is representable.
void via_holder(Holder &h [[clang::noescape]]) { // expected-warning {{uses more than one level of indirection}}
  (void)h;
}

// (A local holder is not exercised here: a default-constructed one draws the
// lost-borrow sentinel, which is a different check and only obscures this one.)
