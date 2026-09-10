// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused \
// RUN:   -Wno-lifetime-safety-unannotated-indirection -verify %s

// A projection extends a loan's access path by one element, and the result is
// projected again whenever the expression that produced it is re-evaluated. Around a
// LOOP that never settles: walking a linked list through a [[clang::lifetimebound]]
// accessor turns `n` into `n.*`, then `n.*.*`, ... -- a fresh, longer loan every
// iteration, so the projection memo never hits and the dataflow has no fixpoint to
// reach. The analysis did not terminate on the four-line function below.
//
// Two consecutive `.*` say nothing more than one: an Interior step absorbs ANY number
// of elements on either side of a path comparison, so `a.*.*` denotes exactly what
// `a.*` does. Collapsing them is therefore exact, and it makes projection idempotent
// here. A depth cap saturates anything else into a wildcard as a backstop.
//
// This test exists to pin the termination; the diagnostics are incidental.

struct Node {
  Node *m_next;
  Node *next() const [[clang::lifetimebound]] { return m_next; } // expected-warning {{could not verify that the return value can be lifetime bound}}
  Node &self() const [[clang::lifetimebound]] { return *m_next; } // expected-warning {{could not verify that the return value can be lifetime bound}}
};

void walk_for(Node *n) {
  for (auto *p = n; p; p = p->next()) {
  }
}

void walk_while(Node *n) {
  while (n)
    n = n->next();
}

void walk_twice(Node *n) {
  for (auto *p = n; p; p = p->next()->next()) {
  }
}

// Alternating a wildcard projection with a named member access. (`Node` carries no
// ownership annotation, so a by-reference parameter of it draws the unknown-ownership
// refusal; unrelated to termination.)
void walk_alternating(Node &n) {
  // expected-warning@+1 {{type 'Node' can hold a borrow but is annotated neither}}
  for (Node *q = &n; q; q = q->self().m_next) {
  }
}

// A plain member walk, which projects a named field rather than a wildcard.
void walk_member(Node *n) {
  for (auto *p = n; p; p = p->m_next) {
  }
}

// Nested loops over the same chain.
void walk_nested(Node *n) {
  for (auto *p = n; p; p = p->next())
    for (auto *q = p; q; q = q->next()) {
    }
}
