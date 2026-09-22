// RUN: %clang_cc1 -fsyntax-only -std=c++20 -I%S/Inputs -Wlifetime-safety-soundness -verify %s

#include "lifetime-analysis.h"

volatile int sink;

// An object survives a mutation of its own contents, so a holder pointing AT a
// container is spared while one pointing INTO it is reported. That decision was
// made by comparing the holder's pointee record against the mutated object's
// record -- and those coincide whenever a container's element type IS the
// container's own record. A tree, a DOM, an AST, a JSON node: a pointer into the
// element buffer has the same pointee record as a pointer at the object, so the
// type test called it "points AT" and said nothing.
//
// The loan already knows better. The result of a `[[clang::lifetimebound]]`
// accessor carries an `Interior` (`.*`) path element -- "somewhere in there, I
// cannot say where" -- so its path is `...scene.*` where a pointer AT the object
// has plain `...scene`. Deciding from the loan rather than the type separates
// them, and does so however the loan is rooted.

struct Tree {
  int id;
  std::vector<Tree> kids;
  Tree *firstKid() [[clang::lifetimebound]] { return &kids[0]; }
  void addKid() { kids.resize(kids.size() + 1); }
};

//===----------------------------------------------------------------------===//
// Rooted at a MEMBER: the loan's root is the `Uninitialized` seed for `scene`.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] Editor {
  Tree *scene;
  Tree *selected;
  void bug() {
    selected = scene->firstKid();
    scene->addKid();     // expected-note {{assumed to be invalidated by this operation}}
    sink = selected->id; // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  }
};

//===----------------------------------------------------------------------===//
// Rooted at `this`: the record comes from the `$this` placeholder instead. Keying
// the decision on the loan covers both; keying it on which root supplied the
// record would have left this one open.
//===----------------------------------------------------------------------===//

struct Tree2 {
  int id;
  std::vector<Tree2> kids;
  Tree2 *firstKid() [[clang::lifetimebound]] { return &kids[0]; }
  void addKid() { kids.resize(kids.size() + 1); }
  void bug() {
    Tree2 *k = firstKid();
    addKid();     // expected-note {{assumed to be invalidated by this operation}}
    sink = k->id; // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  }
};

//===----------------------------------------------------------------------===//
// What must STAY silent: a holder pointing AT the container. This is the false
// positive the record lookup was introduced for, and the case a type-blind fix
// would reopen -- the loan here has no Interior element, so it is still spared.
//===----------------------------------------------------------------------===//

struct PointsAtContainer {
  std::vector<int> *v;
  void grow() { v->push_back(1); }
  void grow_then_read() {
    v->push_back(1);
    sink = (*v)[0];
  }
};

// A whole Tree held AT, mutated through: still not a borrow into its buffer.
struct HoldsTreeAt {
  Tree *t;
  void grow_then_read() {
    t->addKid();
    sink = t->id;
  }
};

// The local and parameter spellings of the same thing.
void at_pointer_in_local(std::vector<int> &vec [[clang::noescape]]) {
  std::vector<int> *p = &vec;
  p->push_back(1);
  sink = (*p)[0];
}

void at_pointer_in_param(std::vector<int> *p [[clang::noescape]]) {
  p->push_back(1);
  sink = (*p)[0];
}
