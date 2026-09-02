// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s
#include "Inputs/lifetime-analysis.h"

using std::string;
using std::string_view;

// A borrow captured into a [[gsl::Owner]] cannot be tracked: an owner is meant to
// own its contents, and a borrow stashed in its (opaque) members is invisible once
// the owner is passed elsewhere. Use a [[gsl::Pointer]] (view) to hold a borrow.
//
// That was checked only for the `this` spelling. The identical capture into a
// NAMED parameter of owner type -- `lifetime_capture_by(r)` with `Record &r` --
// went unrefused, so a hidden friend could stash a borrow in an owner's private
// cache with nothing said. Which owner is named, and how it is spelled, does not
// change the answer, so the capturer's record is now resolved either way and the
// question asked once.
//
// This costs nothing the model supports: the attribute's canonical documented use
// captures into a container of views (`std::set<std::string_view>&`), and such a
// type is already refused for being an owner OF INDIRECTION.

volatile char sink;

//===----------------------------------------------------------------------===//
// Refused: the capturer names an owner.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(int)]] Record {
  void setKeyMember(string_view k [[clang::lifetime_capture_by(this)]]); // expected-warning {{'lifetime_capture_by(this)' names a [[gsl::Owner]] type}}
};

// A named parameter of owner type -- the reported shape.
void setKeyFree(Record &r,
                string_view k [[clang::lifetime_capture_by(r)]]); // expected-warning {{'lifetime_capture_by(r)' names a [[gsl::Owner]] type}}

// A POINTER to an owner is the same capture.
void setKeyPtr(Record *r,
               string_view k [[clang::lifetime_capture_by(r)]]); // expected-warning {{'lifetime_capture_by(r)' names a [[gsl::Owner]] type}}

// By value is still the same annotation being refused; whether the callee's copy
// is the caller's object is not what makes it untrackable.
void setKeyValue(Record r,
                 string_view k [[clang::lifetime_capture_by(r)]]); // expected-warning {{'lifetime_capture_by(r)' names a [[gsl::Owner]] type}}

// On a member function, where the named capturer is a parameter rather than `this`.
struct Holder {
  void stash(Record &r,
             string_view k [[clang::lifetime_capture_by(r)]]); // expected-warning {{'lifetime_capture_by(r)' names a [[gsl::Owner]] type}}
};

// A standard owner named as the capturer is refused for the same reason.
void intoString(string &s,
                string_view k [[clang::lifetime_capture_by(s)]]); // expected-warning {{'lifetime_capture_by(s)' names a [[gsl::Owner]] type}}

//===----------------------------------------------------------------------===//
// Must stay silent: the capturer is not an owner.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] View {
  const char *p;
};

// A view is exactly what should hold a borrow.
void intoView(View &v, string_view k [[clang::lifetime_capture_by(v)]]); // no-warning

// A plain reference parameter that is not an owner.
void intoPointee(const char *&out,
                 string_view k [[clang::lifetime_capture_by(out)]]); // no-warning

// `this` on a non-owner class was never refused and must not start being.
struct PlainHolder {
  const char *p;
  void set(string_view k [[clang::lifetime_capture_by(this)]]); // no-warning
};

// A view class naming its own `this`.
struct [[gsl::Pointer]] ViewHolder {
  const char *p;
  void set(string_view k [[clang::lifetime_capture_by(this)]]); // no-warning
};
