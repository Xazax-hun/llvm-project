// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

volatile char sink;
volatile int isink;

// A borrow rooted at the callee's `$this` placeholder was once not treated as
// invalidated by a mutation reached through a *parameter*, so soundness rested
// entirely on the CALLER-side argument-overlap check flagging the aliasing call
// that makes the two the same object. That check used to miss `this` entirely --
// the `$this` placeholder loan carries neither an issuing expression nor a
// placeholder parameter, so the alias was detected and then dropped as
// unreportable. It is now anchored at the method whose implicit object it stands
// for.
//
// The callee side is now also covered: a parameter placeholder yields its record,
// so a non-const call on a parameter whose type contains the borrowed field is
// treated as possibly invalidating it. That is the same hazard seen from inside,
// and it is targeted -- a parameter of an unrelated type cannot reach the field
// and stays silent. Each case below therefore reports twice: once in the callee at
// the borrow, and once in the caller at the aliasing call.

struct [[gsl::Owner]] Registry {
  vector<string> items;

  void wipe() { items.clear(); }

  void report(Registry *other [[clang::noescape]]) {
    string_view sv = items[0]; // borrow rooted at $this // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    other->wipe();             // frees it when other == this // expected-note {{assumed to be invalidated by this operation}}
    sink = *sv.data();
  }

  // expected-warning@+1 {{implicit object parameter may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  void run() {
    report(this); // expected-note {{assumed to be invalidated by this operation}}
  }
};

// `*this` through a reference parameter.
struct [[gsl::Owner]] RefForm {
  vector<string> items;
  void wipe() { items.clear(); }
  void report(RefForm &other [[clang::noescape]]) {
    string_view sv = items[0]; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    other.wipe(); // expected-note {{assumed to be invalidated by this operation}}
    sink = *sv.data();
  }
  // expected-warning@+1 {{implicit object parameter may be invalidated by an operation}}
  void run() {
    report(*this); // expected-note {{assumed to be invalidated by this operation}}
  }
};

// `this` laundered through a local pointer: the origin still carries the `$this`
// loan, so this is caught by the loans, not by matching the argument expression.
struct [[gsl::Owner]] LocalForm {
  vector<string> items;
  void wipe() { items.clear(); }
  void report(LocalForm *other [[clang::noescape]]) {
    string_view sv = items[0]; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    other->wipe(); // expected-note {{assumed to be invalidated by this operation}}
    sink = *sv.data();
  }
  // expected-warning@+1 {{implicit object parameter may be invalidated by an operation}}
  void run() {
    LocalForm *self = this;
    report(self); // expected-note {{assumed to be invalidated by this operation}}
  }
};

// A free function receiving the same object twice, once mutably.
struct [[gsl::Owner]] FreeForm {
  vector<string> items;
  void wipe() { items.clear(); }
  void run();
};
static void twice(FreeForm *a [[clang::noescape]],
                  FreeForm *b [[clang::noescape]]) {
  string_view sv = a->items[0]; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  b->wipe(); // expected-note {{assumed to be invalidated by this operation}}
  sink = *sv.data();
}
// expected-warning@+1 {{implicit object parameter may be invalidated by an operation}}
void FreeForm::run() {
  twice(this, this); // expected-note {{assumed to be invalidated by this operation}}
}

// Negative: the co-argument is an unrelated owner, not `this`.
struct [[gsl::Owner]] Unrelated {
  vector<int> v;
  void helper(vector<int> &other [[clang::noescape]]) { other.push_back(1); }
  void run(vector<int> &x [[clang::noescape]]) {
    helper(x); // no-warning: 'x' does not alias the receiver
  }
};

// Negative: plain self-mutation. A lone receiver has no co-argument, so there is
// no overlap fact at all -- reporting here would fire on every mutating method.
struct [[gsl::Owner]] SelfMutation {
  vector<int> v;
  void wipe() { v.clear(); }
  void run() {
    wipe(); // no-warning
  }
};

// Negative: two distinct objects of the same type.
struct [[gsl::Owner]] TwoObjects {
  vector<int> v;
  void grow() { v.push_back(1); }
  void take(TwoObjects *o [[clang::noescape]]) { o->grow(); }
};
void two_objects(TwoObjects &a [[clang::noescape]],
                 TwoObjects &b [[clang::noescape]]) {
  a.take(&b); // no-warning: 'b' is a different object from the receiver
}

// Negative: `this` passed twice but nothing is mutated (all const).
struct [[gsl::Owner]] AllConst {
  vector<int> v;
  void look(const AllConst *a [[clang::noescape]],
            const AllConst *b [[clang::noescape]]) const {}
  void run() const {
    look(this, this); // no-warning: no argument is mutated
  }
};

// A mutation of a FIELD does alias a borrow of the whole object: handed the
// object, the callee can borrow into any field of it, including the one being
// mutated -- and `const` does not prevent borrowing. This was a documented
// negative, on the grounds that the mutated argument's static type pins the
// mutation to the field, which is neither the `this` class nor a base of it. But
// that argument only rules out the object being MOVED; it says nothing about a
// borrow taken through the object into the field, which is the hazard below.
// The report is anchored at the method the `$this` placeholder stands for, since
// a widened whole-object borrow has no more precise anchor.
struct [[gsl::Owner]] FieldMutation {
  vector<int> v;
  static void grow(vector<int> &x [[clang::noescape]],
                   const FieldMutation *self [[clang::noescape]]) {
    const int *p = self->v.data(); // borrow of self->v's buffer, through `self`
    x.push_back(1);                // `x` IS self->v -> reallocates, frees it
    isink = *p;                    // dangling
  }
  void run() { // expected-warning {{implicit object parameter may be invalidated by an operation}}
    grow(v, this); // expected-note {{assumed to be invalidated by this operation}}
  }
};

// Positive control for the above: when `this` IS passed mutably, a borrow of its
// own field is the ordinary `f(a, a.b)` hazard and is anchored at the field.
struct [[gsl::Owner]] FieldAndMutableThis {
  vector<int> v;
  static void grow(vector<int> &x [[clang::noescape]],
                   FieldAndMutableThis *self [[clang::noescape]]) {
    self->v.clear();
    x.push_back(1);
  }
  void run() {
    // expected-warning@+1 {{object whose reference is captured may be invalidated by an operation}}
    grow(v, this); // expected-note {{assumed to be invalidated by this operation}}
  }
};
