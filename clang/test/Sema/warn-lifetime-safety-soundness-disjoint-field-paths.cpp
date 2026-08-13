// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

volatile char sink;

// A loan records the whole access path to the storage it borrows, so `w.a` and
// `w.b` are distinguishable rather than both widening to `w`. Two consequences:
//
//   - a borrow of one field survives a mutation of a DISJOINT sibling, which
//     previously had to be reported because the two were indistinguishable;
//   - a borrow INTO a field is still reported when that very field (or anything
//     containing it) is mutated -- containment is a prefix test on the path.
//
// The pairs below differ only in which field is touched, so each negative is
// exactly the positive next to it with the path made to diverge.

struct Comp {
  virtual ~Comp() = default;
  virtual void reload() = 0;
};
void notifyAll(Comp &C [[clang::noescape]]);

struct Cfg : Comp {
  string Text;
  void reload() override { Text = string(); }
};

//===----------------------------------------------------------------------===//
// Reentrancy through a field argument.
//===----------------------------------------------------------------------===//

struct Wrapper {
  Cfg C;
};

// The argument IS the field the borrow points into: mutating it reaches `Text`.
void into_the_field() {
  Wrapper W;
  string_view V = W.C.Text; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  notifyAll(W.C);           // expected-note {{assumed to be invalidated by this operation}}
  sink = *V.data();
}

struct Two {
  Cfg A, B;
};

// Same shape, but the mutated field is a disjoint sibling of the borrowed one.
// `t.A` and `t.B` are siblings under a common root, so neither is a prefix of
// the other and the mutation provably cannot reach the borrow -- even though
// the two fields have the SAME type, which is all the type-based fallback saw.
void disjoint_sibling_same_type() {
  Two T;
  string_view V = T.A.Text;
  notifyAll(T.B); // no-warning: mutating 'B' cannot reach the sibling 'A'
  sink = *V.data();
}

//===----------------------------------------------------------------------===//
// Direct field mutation.
//===----------------------------------------------------------------------===//

struct Fields {
  string S, T;
};
void mutate(Fields &F [[clang::noescape]]);

void mutate_borrowed_field() {
  Fields F;
  string_view V = F.S; // expected-warning {{object whose reference is captured is later invalidated}}
  F.S += "x";          // expected-note {{invalidated here}}
  sink = *V.data();    // expected-note {{later used here}}
}

void mutate_sibling_field() {
  Fields F;
  string_view V = F.S;
  F.T += "x"; // no-warning: 'T' is disjoint from 'S'
  sink = *V.data();
}

// A raw pointer into one field survives a mutation of the sibling too.
void raw_pointer_sibling_field() {
  Fields F;
  const char *P = F.S.data();
  F.T += "x"; // no-warning: 'T' is disjoint from 'S'
  sink = *P;
}

//===----------------------------------------------------------------------===//
// Containment is a PREFIX test, so an enclosing mutation still reports.
//===----------------------------------------------------------------------===//

struct Nested {
  Fields Inner;
};

// The borrow denotes `N.Inner.S`; the mutation names `N.Inner.S` too.
void mutate_nested_borrowed_field() {
  Nested N;
  string_view V = N.Inner.S; // expected-warning {{object whose reference is captured is later invalidated}}
  N.Inner.S += "x";          // expected-note {{invalidated here}}
  sink = *V.data();          // expected-note {{later used here}}
}

// A borrow deeper than the mutation: mutating `N.Inner` (via a whole-object
// operation on it) encloses the borrow of `N.Inner.S`.
void mutate_enclosing_object() {
  Nested N;
  string_view V = N.Inner.S; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  mutate(N.Inner);           // expected-note {{assumed to be invalidated by this operation}}
  sink = *V.data();
}

//===----------------------------------------------------------------------===//
// Containers behave the same way: the element buffer hangs below the field.
//===----------------------------------------------------------------------===//

struct Vecs {
  vector<int> A, B;
};

void container_sibling_is_clean() {
  Vecs V;
  int *P = &V.A[0];
  V.B.push_back(1); // no-warning: 'B' is disjoint from 'A'
  sink = *P ? 1 : 0;
}

void container_same_field_reports() {
  Vecs V;
  int *P = &V.A[0];  // expected-warning {{object whose reference is captured is later invalidated}}
  V.A.push_back(1);  // expected-note {{invalidated here}}
  sink = *P ? 1 : 0; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Distinct VARIABLES are distinct objects, decidable without any field.
//===----------------------------------------------------------------------===//

struct Box {
  string S;
  void grow();
};

void distinct_locals_are_clean() {
  Box A, B;
  string_view V = A.S;
  B.grow(); // no-warning: mutating 'B' cannot reach 'A'
  sink = *V.data();
}

// A reference is another NAME for an object, not another object: its borrows
// are rooted at the referent, so this is still reported.
void reference_alias_reports() {
  Box A;
  Box &R = A;
  string_view V = A.S; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  R.grow();            // expected-note {{assumed to be invalidated by this operation}}
  sink = *V.data();
}

// Two PARAMETERS may be bound to the same object by the caller, so they are not
// provably disjoint and the mutation must still be assumed to reach the borrow.
void params_may_alias(Box &X [[clang::noescape]], Box &Y [[clang::noescape]]) {
  string_view V = X.S; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  Y.grow();            // expected-note {{assumed to be invalidated by this operation}}
  sink = *V.data();
}
