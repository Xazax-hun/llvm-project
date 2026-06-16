// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;

// The argument-overlap check derives the mutated record from the mutated
// argument's STATIC TYPE, not from its propagated loan. A subobject receiver
// (e.g. `outer.grid_.build(...)`) carries a loan that widens to the enclosing
// object's `this` placeholder; deriving the mutated record from that loan would
// make every sibling field of the enclosing object look contained, a false
// positive. The static type pins the mutation to the actual subobject.

struct Inner {
  string buf;
  // Mutates `*this` (its owner field) and only reads `pool`.
  void build(const string &pool [[clang::noescape]]);
};

struct Outer {
  string asteroids_; // sibling field
  Inner grid_;       // sibling field

  // `grid_` (an Inner) is mutated; `asteroids_` is a sibling of the *enclosing*
  // Outer, not a member of Inner -- so it is NOT reachable from the mutated
  // subobject and there is no aliasing hazard.
  void rebuild() {
    grid_.build(asteroids_); // no-warning: disjoint sibling subobjects
  }
};

// Positive control: a borrow of a SUBOBJECT of the actually-mutated argument
// still overlaps -- mutating it may reallocate any owner field it contains.
struct Holder {
  string buf;
  void process(const string &v [[clang::noescape]]) {
    buf.push_back('z');
    (void)v;
  }
  void run() {
    process(buf); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}} expected-note {{assumed to be invalidated by this operation}}
  }
};
