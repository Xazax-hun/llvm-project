// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s
// Also verify the group enables the analysis on its own, not only as part of
// -Wlifetime-safety-soundness.
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-non-invalidating-violation %s 2>&1 | FileCheck %s
// CHECK: promises not to invalidate

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

void unannotated_is_assumed(Pool &p [[clang::noescape]]) {
  int &a = p.v[0]; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  p.untracked_tick(); // expected-note {{assumed to be invalidated by this operation}}
  sink = a;
}

// A method that breaks its promise is reported at the annotation: the promise
// suppresses the invalidation at every CALL SITE, so leaving it unverified hides
// a use-after-free in the caller. The in-body borrow is reported as well.
struct [[gsl::Owner]] Liar {
  vector<int> v;
  // expected-warning@+1 {{invalidates the implicit this parameter, which it promises not to invalidate}}
  [[clang::lifetime_non_invalidating]] int &grow_and_borrow() [[clang::lifetimebound]] {
    int &r = v[0];  // expected-warning {{object whose reference is captured is later invalidated}}
    v.push_back(1); // expected-note {{invalidated here}} expected-note {{invalidated here}}
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

//===----------------------------------------------------------------------===//
// Body verification: the promise must hold for the function's INPUTS.
//
// The attribute suppresses the invalidation at every call site, so an untrue
// promise hides a use-after-free in the caller. Verifying it is what makes the
// attribute safe to trust. Locals are exempt -- they die with the call, so no
// caller borrow can point into one.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner]] Basin {
  vector<int> v;

  // expected-warning@+1 {{invalidates the implicit this parameter, which it promises not to invalidate}}
  [[clang::lifetime_non_invalidating]] void grow() {
    v.push_back(1); // expected-note {{invalidated here}}
  }

  // Transitive: `outer` itself is not flagged -- `inner`'s promise suppresses
  // the fact at this call site, which is exactly what the attribute does. The
  // untruth is still caught, because `inner` is verified against its own body
  // and reports there, so the TU is not silent.
  [[clang::lifetime_non_invalidating]] void outer() { inner(); }
  // expected-warning@+1 {{invalidates the implicit this parameter, which it promises not to invalidate}}
  [[clang::lifetime_non_invalidating]] void inner() {
    v.push_back(1); // expected-note {{invalidated here}}
  }

  // A parameter is an input too, and is named in the diagnostic.
  // expected-warning@+1 {{invalidates parameter 'out', which it promises not to invalidate}}
  [[clang::lifetime_non_invalidating]] void fill(vector<int> &out [[clang::noescape]]) {
    out.push_back(1); // expected-note {{invalidated here}}
  }
};

// The attribute may sit on the DECLARATION while the definition is elsewhere;
// the verifier must find the definition.
struct [[gsl::Owner]] Split {
  vector<int> v;
  // expected-warning@+1 {{invalidates the implicit this parameter, which it promises not to invalidate}}
  [[clang::lifetime_non_invalidating]] void grow();
};
void Split::grow() {
  v.push_back(1); // expected-note {{invalidated here}}
}

//===----------------------------------------------------------------------===//
// Negatives: what the attribute is FOR must stay clean.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner]] Reader {
  vector<int> v;

  // A genuine read-only accessor.
  [[clang::lifetime_non_invalidating]] const int *peek() const [[clang::lifetimebound]] {
    return v.data();
  }

  // Mutating a LOCAL owner is not a broken promise.
  [[clang::lifetime_non_invalidating]] void scratch() const {
    vector<int> tmp;
    tmp.push_back(1); // no-warning: 'tmp' is a local
  }
};
