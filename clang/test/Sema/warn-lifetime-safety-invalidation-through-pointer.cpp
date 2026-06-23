// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-invalidation -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::unique_ptr;
using std::vector;

// A borrow into the object owned by a smart pointer is invalidated when that
// object is mutated THROUGH the pointer -- whether the mutation receiver is a
// dereference (`(*p).m()`), an arrow call (`p->m()`), or an operator on the
// dereferenced object (`*p += ...`). The receiver is not a plain variable, so
// these exercise the owner-lvalue / pointer-to-owner receiver handling.

int deref_operator() {
  unique_ptr<string> p;
  string_view v = *p; // expected-warning {{object whose reference is captured is later invalidated}}
  *p += "x";          // expected-note {{invalidated here}}
  return v.size();    // expected-note {{later used here}}
}

int deref_method() {
  unique_ptr<string> p;
  string_view v = *p; // expected-warning {{object whose reference is captured is later invalidated}}
  (*p).clear();       // expected-note {{invalidated here}}
  return v.size();    // expected-note {{later used here}}
}

int arrow_method() {
  unique_ptr<string> p;
  string_view v = *p; // expected-warning {{object whose reference is captured is later invalidated}}
  p->clear();         // expected-note {{invalidated here}}
  return v.size();    // expected-note {{later used here}}
}

int arrow_vector() {
  unique_ptr<vector<int>> p;
  auto it = p->begin(); // expected-warning {{object whose reference is captured is later invalidated}}
  p->push_back(1);      // expected-note {{invalidated here}}
  return *it;           // expected-note {{later used here}}
}

// Reassigning the owned object through the pointer also invalidates the view.
int deref_assign() {
  unique_ptr<string> p;
  string_view v = *p; // expected-warning {{object whose reference is captured is later invalidated}}
  *p = "y";           // expected-note {{invalidated here}}
  return v.size();    // expected-note {{later used here}}
}

// reset() / move-assign destroy the pointee.
int reset_destroys() {
  unique_ptr<string> p;
  string_view v = *p; // expected-warning {{object whose reference is captured is later invalidated}}
  p.reset();          // expected-note {{invalidated here}}
  return v.size();    // expected-note {{later used here}}
}

// A reference bound to the pointee is invalidated the same way.
int ref_to_pointee() {
  unique_ptr<string> p;
  string &r = *p;     // expected-warning {{object whose reference is captured is later invalidated}}
  p.reset();          // expected-note {{invalidated here}}
  return (int)*r.data(); // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Negative: a const method through the pointer does not invalidate.
//===----------------------------------------------------------------------===//

int const_method_ok() {
  unique_ptr<string> p;
  string_view v = *p;
  (void)p->c_str(); // no-warning (const)
  return v.size();
}

//===----------------------------------------------------------------------===//
// Negative: a recognized NON-INVALIDATING accessor (`operator[]`, `at()`,
// `front()`, `data()`, ...) does not reallocate, so it does not invalidate a
// borrow into the pointee -- even when it is a NON-const overload reached
// through a pointer (`p->at(i)`, `(*p)[i]`). The non-invalidating allow-list
// must be consulted on the pointee record, not only on a direct owner receiver.
//===----------------------------------------------------------------------===//

int nonconst_accessor_through_pointer_ok() {
  unique_ptr<vector<char>> p;
  char *e = &(*p)[0];  // borrow into the pointee vector
  (void)p->at(0);      // non-const at() through the pointer -- not a mutation
  (void)(*p)[0];       // non-const operator[] through the pointer -- not a mutation
  return *e;           // no-warning
}
