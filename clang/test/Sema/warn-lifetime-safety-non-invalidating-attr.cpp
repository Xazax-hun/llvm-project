// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::vector;

volatile int sink;

// Any non-const member call on an owner is conservatively assumed to reallocate
// it, invalidating borrows into it. That is necessary in general, but wrong for a
// method that only mutates parts no borrow can point into. Equivalent std
// accessors (`v[i]`, `v.at(i)`, `v.data()`, ...) are recognized by name;
// `[[clang::lifetime_non_invalidating]]` extends that to user-defined owners,
// which cannot be recognized by name.

struct [[gsl::Owner]] Pool {
  vector<int> v;
  int counter = 0;

  // Bumps a scalar; cannot reallocate `v`, so no borrow into the pool dies.
  [[clang::lifetime_non_invalidating]] void tick() { counter += 1; }

  // Not annotated: conservatively assumed to invalidate.
  void untracked_tick() { counter += 1; }
};

void annotated_is_exempt(Pool &p [[clang::noescape]]) {
  int &a = p.v[0];
  p.tick(); // no-warning: promised not to invalidate
  sink = a;
}

// expected-warning@+1 {{parameter may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
void unannotated_is_assumed(Pool &p [[clang::noescape]]) {
  int &a = p.v[0];
  p.untracked_tick(); // expected-note {{assumed to be invalidated by this operation}}
  sink = a;
}

// The attribute suppresses only the *assumed* invalidation. A mutation the
// analysis can actually see is still reported, so the promise cannot hide a
// reallocation performed in the annotated method's own body.
struct [[gsl::Owner]] Liar {
  vector<int> v;
  [[clang::lifetime_non_invalidating]] int &grow_and_borrow() [[clang::lifetimebound]] {
    int &r = v[0];  // expected-warning {{object whose reference is captured is later invalidated}}
    v.push_back(1); // expected-note {{invalidated here}}
    return r;       // expected-note {{later used here}}
  }
};

// Nor does it hide a real invalidation performed by the caller.
void real_invalidation_still_reported(Pool &p [[clang::noescape]]) {
  int &a = p.v[0];  // expected-warning {{object whose reference is captured is later invalidated}}
  p.v.push_back(1); // expected-note {{invalidated here}}
  sink = a;         // expected-note {{later used here}}
}

// It also applies to a method of a record that merely *contains* owners, where the
// name-based std allow-list is gated on the receiver being an owner itself.
struct Holder {
  vector<int> v;
  int counter = 0;
  [[clang::lifetime_non_invalidating]] void tick() { counter += 1; }
};

void holder_is_exempt(Holder &h [[clang::noescape]]) {
  int &a = h.v[0];
  h.tick(); // no-warning
  sink = a;
}
