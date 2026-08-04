// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

volatile char sink;

// A borrow rooted at the callee's `$this` placeholder is deliberately NOT treated
// as invalidated by a mutation reached through a *parameter*: that would fire on
// every method that mutates itself. The rule is sound only because the CALLER-side
// argument-overlap check flags the aliasing call that makes the two the same
// object. That check used to miss `this` entirely -- the `$this` placeholder loan
// carries neither an issuing expression nor a placeholder parameter, so the alias
// was detected and then dropped as unreportable. It is now anchored at the method
// whose implicit object it stands for.

struct [[gsl::Owner]] Registry {
  vector<string> items;

  void wipe() { items.clear(); }

  void report(Registry *other [[clang::noescape]]) {
    string_view sv = items[0]; // borrow rooted at $this
    other->wipe();             // frees it when other == this
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
    string_view sv = items[0];
    other.wipe();
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
    string_view sv = items[0];
    other->wipe();
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
  string_view sv = a->items[0];
  b->wipe();
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

// Negative: a mutation of a FIELD does not alias a borrow of the whole object.
// Both loans widen to the same `$this` root, so deciding the alias by loan
// identity would be a false positive; the mutated argument's static type pins the
// mutation to the field, which is neither the `this` class nor a base of it.
// (`self` is const, so the only mutated argument is the field `v`.)
struct [[gsl::Owner]] FieldMutation {
  vector<int> v;
  static void grow(vector<int> &x [[clang::noescape]],
                   const FieldMutation *self [[clang::noescape]]) {
    x.push_back(1);
    (void)self;
  }
  void run() {
    grow(v, this); // no-warning: mutating a field does not move the object
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
