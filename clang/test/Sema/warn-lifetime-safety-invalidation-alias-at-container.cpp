// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

// A mutation of a container's CONTENTS does not endanger a pointer or reference AT
// the container: the object survives, only borrows INTO it move. The invalidation
// check treated a loan naming the mutated storage exactly as invalidated, so any
// two mutations through a reference or pointer parameter reported the parameter as
// invalidated by the first -- `v.push_back(1); v.push_back(2);` warned, with
// nothing dangling. A local object was spared only incidentally: its receiver is a
// throwaway expression origin re-issued per call, so no origin survived to be found
// live holding the invalidated loan.
//
// A loan naming the storage EXACTLY denotes the object; only one strictly below it
// points into what moved. Two things make that usable, and both are needed:
//
//  - A borrow INTO an object records an Interior (`.*`) step, so it says "somewhere
//    inside `s`" rather than "`s`". That is what separates a view built from an
//    owner from a reference bound to it, which otherwise carry the identical loan.
//
//  - The exemption is granted only on positively recognising a pointer or reference
//    whose pointee record IS the mutated record. This is what keeps the check
//    sound, and it cannot be replaced by the paths alone: an origin with no
//    recorded type, a member seeded with an Uninitialized placeholder, and a heap
//    allocation have no expression to project from, and a wildcard-vs-wildcard pair
//    (`subdirs.*` held while `subdirs.*` is mutated) compares EQUAL even though the
//    holder does point into what moved. A holder the predicate cannot classify
//    reports, so its gaps cost a false positive rather than a missed bug.
//
// A DEALLOCATION is the opposite case: it destroys the object, so a pointer at it
// is exactly what dangles.

volatile int sink;

//===----------------------------------------------------------------------===//
// Must stay silent: the alias points AT the container.
//===----------------------------------------------------------------------===//

// The reported shape.
void two_pushes_ref(vector<int> &v [[clang::noescape]]) {
  v.push_back(1);
  v.push_back(2); // no-warning
}

// The pointer spelling was equally affected.
void two_pushes_ptr(vector<int> *v [[clang::noescape]]) {
  v->push_back(1);
  v->push_back(2); // no-warning
}

// A local alias of a local container.
void local_alias() {
  vector<int> vv;
  vector<int> &r = vv;
  vector<int> *p = &vv;
  vv.push_back(1);
  r.push_back(2);
  p->push_back(3); // no-warning
  sink = (int)r.size() + (int)p->size();
}

//===----------------------------------------------------------------------===//
// Must still report: the holder points INTO the container.
//===----------------------------------------------------------------------===//

// A raw pointer to an element.
int element_pointer(vector<int> &v [[clang::noescape]]) { // expected-warning {{later invalidated}}
  int *p = &v[0];
  v.push_back(1); // expected-note {{invalidated here}}
  return *p;      // expected-note {{later used here}}
}

// An iterator.
int iterator(vector<int> &v [[clang::noescape]]) { // expected-warning {{later invalidated}}
  auto it = v.begin();
  v.push_back(1);      // expected-note {{invalidated here}}
  return *it;          // expected-note {{later used here}}
}

// A view of an owner. Its type is a gsl::Pointer, which is recognised as pointing
// into rather than at.
char view_of_owner(string &s [[clang::noescape]]) { // expected-warning {{later invalidated}}
  string_view sv = s;
  s.push_back('c');   // expected-note {{invalidated here}}
  return sv.data()[0]; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must still report: the holder cannot be classified, so it is not exempted.
// These are where a loan- or path-shape rule would have gone quiet instead --
// nothing in the loans distinguishes them from a pointer at the container.
//===----------------------------------------------------------------------===//

// A closure capturing a reference INTO the container. Not a pointer type and not a
// view, so the exemption must not be granted.
void closure_capture(vector<int> &v [[clang::noescape]]) { // expected-warning {{later invalidated}}
  // expected-warning@+1 {{lifetime safety cannot track local variable 'r' here}}
  auto f = [&r = v[0]] { (void)r; };
  v.push_back(2);                    // expected-note {{invalidated here}}
  (void)f;                           // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// A DEALLOCATION destroys the object, so a pointer AT it is what dangles.
//===----------------------------------------------------------------------===//

struct Obj {
  int id;
};

void deleted_object() {
  Obj *h = new Obj; // expected-warning {{allocated object does not live long enough}}
  Obj *alias = h;
  delete h;              // expected-note {{freed here}}
  sink = alias->id;      // expected-note {{later used here}}
}
