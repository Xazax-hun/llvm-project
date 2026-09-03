// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A borrow of storage that dies with the call, stored into an object the CALLER
// owns, is reported at the store: the destination outlives the call by definition
// and the source does not, so no liveness question arises.
//
// Which sources count was decided by asking "is this a parameter?", excluding all
// of them on the grounds that a parameter's own storage escaping is the noescape
// question. That holds for a REFERENCE or POINTER parameter, which denotes storage
// the caller owns -- but not for a BY-VALUE parameter, whose parameter object dies
// with the call just as a local does. Copying such a parameter into a local first
// was reported all along, so only the root of the borrow differed.
//
// This is not specific to owners: the address of a by-value scalar parameter is
// the same hazard.

volatile char sink;

//===----------------------------------------------------------------------===//
// Caught: the borrowed storage dies with the call.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(char)]] Cache {
  friend void from_local(Cache &c [[clang::noescape]]);
  friend void from_by_value(Cache &c [[clang::noescape]], string v);
  friend void from_by_value_copied(Cache &c [[clang::noescape]], string v);
  friend void from_by_value_scalar(Cache &c [[clang::noescape]], char ch);
  friend void from_reference(Cache &c [[clang::noescape]],
                             const string &r [[clang::noescape]]);
  friend void from_pointer(Cache &c [[clang::noescape]],
                           const char *p [[clang::noescape]]);
  friend void from_static(Cache &c [[clang::noescape]]);

private:
  const char *d_ = ""; // expected-note 4 {{this field dangles}}
};

// A callee local, which was reported all along.
void from_local(Cache &c [[clang::noescape]]) {
  string l("local");
  c.d_ = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' escapes to the field 'd_'}}
}

// The reported shape: a by-value parameter of owner type. The parameter object is
// a copy that dies with the call, so the borrow of its buffer dangles.
void from_by_value(Cache &c [[clang::noescape]], string v) {
  c.d_ = v.c_str(); // expected-warning {{stack memory associated with parameter 'v' escapes to the field 'd_'}}
}

// The same store with the parameter copied into a local first. This was caught
// before the fix, and it is the discriminator: only the borrow's root differs.
void from_by_value_copied(Cache &c [[clang::noescape]], string v) {
  string l = v;
  c.d_ = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' escapes to the field 'd_'}}
}

// Nothing about this needs an owner: a by-value scalar parameter dies just the
// same, and its address is just as dangling.
void from_by_value_scalar(Cache &c [[clang::noescape]], char ch) {
  c.d_ = &ch; // expected-warning {{stack memory associated with parameter 'ch' escapes to the field 'd_'}}
}

//===----------------------------------------------------------------------===//
// Must stay silent here: a caller-owned source is the noescape question, and it
// is answered by its own check with its own wording.
//===----------------------------------------------------------------------===//

// A reference parameter denotes the caller's object, so this is an escape of that
// parameter, not of storage that dies with the call.
void from_reference(Cache &c [[clang::noescape]],
                    const string &r [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  c.d_ = r.c_str(); // expected-note {{escapes into an object the caller owns here}}
}

// Likewise through a pointer parameter.
void from_pointer(Cache &c [[clang::noescape]],
                  const char *p [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  c.d_ = p; // expected-note {{escapes into an object the caller owns here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A static outlives every call, so nothing dangles.
void from_static(Cache &c [[clang::noescape]]) {
  static const char kImmortal[] = "immortal";
  c.d_ = kImmortal; // no-warning
}

// A store into a LOCAL object is no escape: that object's own expiry checks it.
struct [[gsl::Owner(char)]] LocalDest {
  friend void into_local_object(string v);
  char read() const { return d_[0]; }

private:
  const char *d_ = "";
};

void into_local_object(string v) {
  LocalDest d;
  d.d_ = v.c_str(); // no-warning: `d` dies before `v` does
  sink = d.read();
}
