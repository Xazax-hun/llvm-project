// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// The argument-overlap check asks whether a co-argument's borrow aliases the
// storage passed mutably, and it decided that by storage CONTAINMENT -- but only
// in one direction: the borrow being AT OR BELOW the mutated storage, which is
// `f(a, a.b)` read as "mutating `a` may reallocate the field `a.b` borrows".
//
// The other direction was missing. A co-argument that borrows an object
// CONTAINING the mutated storage reaches it just as surely: handed the whole
// `Model`, the callee borrows `m.data`'s buffer through `m`, and the mutation of
// `m.data` frees it. So `refill(m, m.data)` was silent while `refill(v, v)` and a
// view alongside its owner were both caught -- and no annotation can express
// that two parameters must not alias, which is why this check exists.
//
// Both directions now count. Disjoint subobjects still do not: neither `m.a` nor
// `m.b` is a prefix of the other.

volatile char sink;

//===----------------------------------------------------------------------===//
// Caught: a co-argument borrows a container of the mutated storage.
//===----------------------------------------------------------------------===//

struct Model {
  string data;
};

// The reported shape: the container by const reference, its member owner mutably.
void refill(const Model &m [[clang::noescape]], string &out [[clang::noescape]]) {
  const char *p = m.data.data(); // borrows m.data's buffer, reached through `m`
  out.push_back('z');            // `out` IS m.data -> reallocates, frees it
  sink = *p;                     // dangling
}

void call_container_first() {
  Model m;
  refill(m, m.data); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}} expected-note {{assumed to be invalidated by this operation}}
}

// Argument ORDER is not what makes it an overlap.
void refill_swapped(string &out [[clang::noescape]],
                    const Model &m [[clang::noescape]]) {
  const char *p = m.data.data();
  out.push_back('z');
  sink = *p;
}

void call_member_first() {
  Model m;
  refill_swapped(m.data, m); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}} expected-note {{assumed to be invalidated by this operation}}
}

// Reached through a pointer to the container. This one is covered by a
// different, earlier rule and must stay that way: passing the member mutably
// requires a NON-const pointer to the container, and forming a non-const alias
// to an owner that is borrowed already reports at the alias, before the call.
void call_through_pointer() {
  Model m;
  Model *mp = &m; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  refill(*mp, mp->data); // expected-note {{assumed to be invalidated by this operation}}
}

// Containment is transitive: the mutated storage may be nested any depth below
// the borrowed container.
struct Outer {
  Model inner;
};

void call_nested() {
  Outer o;
  refill(o.inner, o.inner.data); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}} expected-note {{assumed to be invalidated by this operation}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A separate owner is not reachable from the container.
void call_unrelated_owner() {
  Model m;
  string other;
  refill(m, other); // no-warning
}

// Disjoint subobjects: the borrowed one is neither at nor below the mutated one,
// nor does it contain it.
struct TwoFields {
  string a;
  string b;
};

void two_fields(const string &borrowed [[clang::noescape]],
                string &mutated [[clang::noescape]]) {
  const char *p = borrowed.data();
  mutated.push_back('z');
  sink = *p;
}

void call_disjoint_siblings() {
  TwoFields t;
  two_fields(t.a, t.b); // no-warning
}

// The container passed alongside a DISJOINT sibling of the mutated field is
// still an overlap -- the container reaches both -- but a container passed
// alongside an owner it does not contain is not.
void call_container_and_foreign() {
  Model m;
  Outer o;
  refill(m, o.inner.data); // no-warning: `m` does not contain `o.inner.data`
}
