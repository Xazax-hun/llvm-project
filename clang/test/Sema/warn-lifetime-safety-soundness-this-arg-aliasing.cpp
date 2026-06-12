// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-assumed-invalidation -verify %s

#include "Inputs/lifetime-analysis.h"
using std::unique_ptr;

// The argument-overlap check relates a call's arguments to each other to catch
// aliasing (e.g. `f(s, s)`); it must also relate an explicit argument to the
// IMPLICIT object (`this`) of the same call. Here a lifetimebound accessor of
// the receiver is passed as an argument while the receiver itself is the mutated
// owner: a non-const member call that reallocates `this` may invalidate the
// borrow the argument holds into it.

struct [[gsl::Owner]] Inner {
  int x;
};
struct [[gsl::Owner]] Holder {
  unique_ptr<Inner> p;
  int &get() [[clang::lifetimebound]] { return p->x; } // borrows *this->p
  void reseat() { p = unique_ptr<Inner>(); }           // frees the old Inner
  void process(int &borrowed [[clang::noescape]]) {
    reseat();
    (void)borrowed;
  }
};

void this_alias_warns() {
  Holder h;
  h.process(h.get()); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
                      // expected-note@-1 {{assumed to be invalidated by this operation}}
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// Distinct receivers: the borrow comes from a different object than the mutated
// one, so there is no aliasing.
void distinct_receivers(Holder &a, Holder &b) {
  a.process(b.get()); // no-warning
}
