// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused \
// RUN:   -Wno-lifetime-safety-unannotated-indirection -verify %s
//
// The unannotated-indirection demand is switched off: every `vector<int> &`
// parameter here draws it, it is an orthogonal annotation requirement, and it is
// not what is under test.

#include "Inputs/lifetime-analysis.h"
using std::vector;

// `[[clang::lifetime_non_invalidating]]` on a PARAMETER makes the same statement the
// method form makes about the implicit object: the call does not invalidate borrows
// into what that parameter refers to.
//
// A free function had no way to say it. Any non-const reference to an owner is
// conservatively assumed to reallocate, the name-based allow-list describes std
// methods only, and the method form needs an implicit object -- so a function that
// writes its argument's elements in place was permanently assumed to invalidate it.
//
// Per-parameter, not per-function: an unannotated sibling parameter is still assumed
// to be invalidated. And verified against the body, like the method form -- an untrue
// promise suppresses the operation at every call site, which is exactly what hides a
// use-after-free from the caller.

volatile int sink;

//===----------------------------------------------------------------------===//
// The promise is honoured, so callers are not warned.
//===----------------------------------------------------------------------===//

// Writes elements in place; never reallocates.
void normalize(vector<int> &v [[clang::lifetime_non_invalidating]]) { // no-warning
  for (unsigned i = 0; i < v.size(); ++i)
    v[i] = 0;
}

void caller_keeps_borrow(vector<int> &data [[clang::noescape]]) {
  int *p = &data[0];
  normalize(data);
  sink = *p; // no-warning: `normalize` promised not to invalidate `data`
}

// Without the annotation the same call is assumed to invalidate -- the two must
// differ, or the attribute is doing nothing.
void unannotated(vector<int> &v);

void caller_without_promise(vector<int> &data [[clang::noescape]]) { // expected-warning {{parameter may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int *p = &data[0];
  unannotated(data); // expected-note {{assumed to be invalidated by this operation}}
  sink = *p;
}

//===----------------------------------------------------------------------===//
// The promise is verified against the body.
//===----------------------------------------------------------------------===//

// expected-warning@+1 {{this function invalidates parameter 'v', which its '[[clang::lifetime_non_invalidating]]' annotation promises not to invalidate}}
void grows(vector<int> &v [[clang::lifetime_non_invalidating]]) {
  v.push_back(1); // expected-note {{invalidated here}}
}

// Assumed invalidations are not exempt either: reallocating by calling an unexamined
// non-const helper is exactly the shape the attribute makes invisible.
void helper(vector<int> &v);

// expected-warning@+1 {{this function invalidates parameter 'v', which its '[[clang::lifetime_non_invalidating]]' annotation promises not to invalidate}}
void delegates(vector<int> &v [[clang::lifetime_non_invalidating]]) {
  helper(v); // expected-note {{invalidated here}}
}

//===----------------------------------------------------------------------===//
// Per-parameter, not per-function.
//===----------------------------------------------------------------------===//

// Only `a` promises anything, and only `b` is mutated.
void mixed(vector<int> &a [[clang::lifetime_non_invalidating]],
           vector<int> &b) { // no-warning
  b.push_back(1);
}

// Each annotated parameter is judged on its own, and each is reported.
void both(vector<int> &a [[clang::lifetime_non_invalidating]], // expected-warning {{this function invalidates parameter 'a'}}
          vector<int> &b [[clang::lifetime_non_invalidating]]) { // expected-warning {{this function invalidates parameter 'b'}}
  a.push_back(1); // expected-note {{invalidated here}}
  b.push_back(2); // expected-note {{invalidated here}}
}

// Mutating a LOCAL is not a violation: it dies with the call, so no caller borrow can
// point into it.
void local_only(vector<int> &v [[clang::lifetime_non_invalidating]]) { // no-warning
  vector<int> tmp;
  tmp.push_back(1);
  sink = (int)v.size();
}

//===----------------------------------------------------------------------===//
// Subjects: a member function or a parameter, and nothing else.
//===----------------------------------------------------------------------===//

struct Subjects {
  // On the function it is written BEFORE the declaration; on a parameter it
  // follows the parameter, like lifetimebound.
  [[clang::lifetime_non_invalidating]] void method();           // ok
  void param(int *q [[clang::lifetime_non_invalidating]]);      // ok
};

void free_param(int *q [[clang::lifetime_non_invalidating]]);   // ok

// expected-error@+1 {{'clang::lifetime_non_invalidating' attribute only applies to functions and parameters}}
[[clang::lifetime_non_invalidating]] int not_a_function;
