// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::vector;

// Calling a lambda that captured `this` can mutate the enclosing object through any
// member the body reaches. The closure is the receiver, so only IT was invalidated;
// and the body is analyzed as its own function, where the captured `this` is not the
// object either. The mutation was therefore attributed to nothing, and wrapping a
// statement in an immediately-invoked lambda silenced the report the identical
// unwrapped statement produced.
//
// Assumed, like every other call whose effect on an owner cannot be seen: the body
// may not mutate anything, and then this costs a false positive rather than a missed
// reallocation.

volatile int sink;

struct [[gsl::Pointer]] Warmer {
  // The direct case below also reports the member itself, which is unrelated to the
  // lambda spellings.
  // expected-warning@+2 {{borrow held by this member which escapes to a field is later invalidated}}
  // expected-note@+1 {{this field dangles}}
  vector<int> *pv;

  // The control: the same mutation written directly.
  void warm_direct() const {
    // expected-note@+2 {{invalidated here}}
    // expected-warning@+1 {{mutating an owner through a pointer or reference member of a const-qualified}}
    pv->push_back(1);
  }

  // The reported shape: an immediately-invoked lambda.
  void warm_iife() const {
    [this] { pv->push_back(1); }(); // expected-warning {{mutating an owner through a pointer or reference member of a const-qualified}}
  }

  // Stored first, then called.
  void warm_stored() const {
    auto f = [this] { pv->push_back(1); };
    f(); // expected-warning {{mutating an owner through a pointer or reference member of a const-qualified}}
  }
};

//===----------------------------------------------------------------------===//
// A borrow taken before the call is invalidated by it.
//===----------------------------------------------------------------------===//

struct Holder {
  vector<int> own;

  void grow_via_lambda() {
    // The invalidation is ASSUMED -- the call's effect on the object cannot be seen
    // -- so the report is the assumed variant.
    // expected-warning@+1 {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    int *p = &own[0];
    // expected-warning@+2 {{lifetime safety cannot track this value here}}
    // expected-note@+1 {{assumed to be invalidated by this operation}}
    [this] { own.push_back(1); }();
    sink = *p;
  }
};

//===----------------------------------------------------------------------===//
// Must stay silent: no `this` capture, so the object is out of reach.
//===----------------------------------------------------------------------===//

struct Quiet {
  vector<int> own;

  void no_this_capture() {
    int *p = &own[0];
    vector<int> other;
    [&other] { other.push_back(1); }(); // no-warning: cannot reach `own`
    sink = *p;
  }

  // A lambda called on an object that is not the enclosing one.
  void unrelated() {
    int *p = &own[0];
    auto f = [] { return 1; };
    sink = f() + *p; // no-warning
  }
};
