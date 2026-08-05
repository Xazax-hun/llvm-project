// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

volatile char sink;

// The argument-overlap check must reach through a `gsl::Pointer` receiver: a
// wrapper that points AT an owner (here `string* p`, bound via a
// `lifetimebound` constructor) holds the borrow into that owner on
// its POINTEE origin, one indirection level in -- not on the wrapper's own
// origin. A non-const method that mutates the pointee owner while a co-argument
// view aliases it is an aliasing hazard, even though the view is not live after
// the call (so the liveness-based invalidation pass misses it).

struct [[gsl::Pointer]] W {
  // The wrapper's borrow lives in this member, so the mutation through it is also
  // reported against the member (in addition to the argument-overlap hazard).
  string *p; // expected-warning {{borrow held by this member which escapes to a field is later invalidated}} expected-note {{this field dangles}}
  W(string &s [[clang::lifetimebound]]); // captures &s into the pointee origin
  void grow_and_use(string_view v [[clang::noescape]]) {
    p->push_back('z'); // reallocates *p // expected-note {{invalidated here}}
    sink = *v.data();  // v aliased *p -> dangling
  }
};

void run() {
  string s;
  W w(s);
  string_view v = s; // view aliases s == *w.p
  // expected-warning@-1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  w.grow_and_use(v); // expected-note {{assumed to be invalidated by this operation}}
}

// Negative: a view of a SEPARATE owner the wrapper does not point at is not
// reachable through the receiver's pointee -> no overlap hazard.
void run_ok() {
  string s;
  W w(s);
  string other;
  string_view v = other;
  w.grow_and_use(v); // no-warning: 'other' is not the wrapper's pointee
}
