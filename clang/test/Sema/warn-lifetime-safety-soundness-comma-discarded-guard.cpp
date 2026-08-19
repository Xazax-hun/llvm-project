// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused-value -verify %s

#include "Inputs/lifetime-analysis.h"

using std::vector;

// Destroying a guard that holds a borrow is modelled as a use of it plus an
// assumed invalidation, since the analysis cannot see what the destructor does.
// A temporary reaches that model through one of two CFG elements, so the
// handler for the CFGTemporaryDtor form has to tell a DISCARDED temporary (its
// own to model) from one CONSUMED as a subexpression (modelled where it is
// consumed, and reporting it here too would double-fire).
//
// That test bailed on any enclosing expression, which is a blocklist of the
// contexts that discard a value -- and the comma operator was missing from it.
// A built-in comma is not a call, so it consumes nothing: `(Guard{&v}, 0)`
// discards the guard exactly as `Guard{&v};` does, but looked consumed. Every
// position that spells a discarded value with a comma was silent with a live
// borrow into the owner.

struct [[gsl::Pointer]] Grower {
  vector<int> *vec;
  int tag = 0;
  ~Grower();
};

//===----------------------------------------------------------------------===//
// The comma positions. A comma's LEFT operand is always discarded; its RIGHT
// operand is discarded exactly when the comma itself is.
//===----------------------------------------------------------------------===//

void comma_left() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  (Grower{&v}, 0); // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

void comma_right() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  (0, Grower{&v}); // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

void comma_in_initializer() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  int x = (Grower{&v}, 1); // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
  (void)x;
}

void comma_in_for_increment() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  for (int i = 0; i < 1; ++i, Grower{&v}) { // expected-note {{assumed to be invalidated by this operation}}
  }
  (void)r;
}

void comma_in_condition() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  if (Grower{&v}, false) { // expected-note {{assumed to be invalidated by this operation}}
  }
  (void)r;
}

void comma_in_while_condition() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  while (Grower{&v}, false) { // expected-note {{assumed to be invalidated by this operation}}
  }
  (void)r;
}

void comma_in_switch_condition() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  switch (Grower{&v}, 0) { // expected-note {{assumed to be invalidated by this operation}}
  default:;
  }
  (void)r;
}

//===----------------------------------------------------------------------===//
// A temporary CONSUMED as an argument is modelled where it is consumed, so it
// must be reported exactly once -- including through a comma, whose value is
// then used.
//===----------------------------------------------------------------------===//

int take(Grower g [[clang::noescape]]);

void consumed_through_comma() {
  vector<int> v;
  v.push_back(42);
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int &r = v[0];
  int x = take((0, Grower{&v})); // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
  (void)x;
}

//===----------------------------------------------------------------------===//
// A guard that cannot mutate the owner stays silent in a comma too.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] ConstGuard {
  const vector<int> *vec;
  int tag = 0;
  ~ConstGuard();
};

void const_guard_comma_silent() {
  vector<int> v;
  v.push_back(42);
  int &r = v[0];
  (ConstGuard{&v}, 0); // no-warning
  (void)r;
}

// A comma with no temporary at all.
void plain_comma_silent() {
  vector<int> v;
  v.push_back(42);
  int &r = v[0];
  (void)(1, 2); // no-warning
  (void)r;
}
