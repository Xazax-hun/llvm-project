// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

// Reentrancy: a method whose own object is destroyed by a call it makes.
//
//   void Node::update() { g_scene.killAll(); use(label); }
//
// where the global scene owns the nodes. Nothing covered this. The mutation of
// the scene IS recognized (an InvalidateOrigin on the global), but the borrow
// being read is rooted at the `$this` placeholder -- a caller-scope root the
// intra-procedural analysis never expires -- and no edge says the scene owns
// `*this`, so the invalidation never reaches it.
//
// The model already demands an annotation on every reference PARAMETER that can
// hold a borrow, which is what catches the same shape written with the scene as
// a parameter. `this` had no equivalent demand, and `global.method()` was
// exempt from the mutable-global rule, so the two exemptions met here.
//
// Binding a mutable-owner global to a NON-CONST implicit object is a mutable
// borrow of it, with all the reach the callee's body has. A const method keeps
// the exemption.

struct Node {
  std::string label;
  void update();
};

struct Scene {
  std::vector<std::string> names; // an owner: reallocatable, destroys contents
  void killAll();                 // non-const
  bool empty() const;             // const: cannot reallocate
};

Scene g_scene;

//===----------------------------------------------------------------------===//
// The reported shape and its neighbours.
//===----------------------------------------------------------------------===//

void Node::update() {
  g_scene.killAll(); // expected-warning {{borrows from a mutable global or static object}}
  (void)label;
}

// Not a method: the same mutable borrow of the global, reported the same way.
void free_function() {
  g_scene.killAll(); // expected-warning {{borrows from a mutable global or static object}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A const method cannot reallocate, and any borrow it returns is checked at its
// own use/escape. This is the interaction the model permits.
void const_call() {
  (void)g_scene.empty(); // no-warning
}

// A global with no owner anywhere has no reallocatable storage and nothing to
// destroy, so a non-const call on it is not a mutable borrow of an owner.
struct Plain {
  int a;
  void bump();
};
Plain g_plain;

void plain_global_call() {
  g_plain.bump(); // no-warning
}

// A LOCAL of the same owning type is not a global: its mutation is ordinary,
// and a borrow into it is tracked precisely by the invalidation rules.
void local_scene() {
  Scene s;
  s.killAll(); // no-warning
}
