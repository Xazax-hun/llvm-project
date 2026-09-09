// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-soundness -Wno-unused \
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
// The METHOD form is about the implicit object, and only that.
//===----------------------------------------------------------------------===//

struct Holder {
  int counter = 0;
  vector<int> own;

  // Leaves the object alone and reallocates an ARGUMENT. The call site still
  // assumes the argument is invalidated (see the caller below), so this
  // contradicts nothing the annotation claims.
  [[clang::lifetime_non_invalidating]] void tick(vector<int> &v) { // no-warning
    counter += 1;
    v.push_back(1);
  }
};

void method_promise_covers_the_object(Holder &h [[clang::noescape]],
                                      vector<int> &w [[clang::noescape]]) { // expected-warning {{parameter may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int *ph = &h.own[0];
  int *pw = &w[0];
  h.tick(w);   // expected-note {{assumed to be invalidated by this operation}}
  sink = *ph;  // no-warning: the object was promised untouched
  sink = *pw;  // the argument was not, and is reported above
}

// For a C++23 explicit object member function the object IS a parameter, so the
// method promise still has to recognise it as the object.
struct Deducing {
  vector<int> own;
  [[clang::lifetime_non_invalidating]] void grow(this Deducing &self) { // expected-warning {{invalidates parameter 'self', which it promises not to invalidate}}
    self.own.push_back(1); // expected-note {{invalidated here}}
  }
};

//===----------------------------------------------------------------------===//
// The promise is consumed at the call site against the STATICALLY resolved
// callee, so every declaration that a call can resolve to must carry it.
//===----------------------------------------------------------------------===//

// A virtual override that drops it is a hole: the call through the base
// suppresses the invalidation, and the override's body carries no promise to
// verify. Requiring the override to repeat it also routes its body through the
// verifier -- the same rule the method-level promise already follows.
struct VBase {
  virtual ~VBase() = default;
  virtual void touch(vector<int> &v [[clang::lifetime_non_invalidating]]) const {}
};

struct VDerived : VBase {
  // expected-warning@+2 {{this override drops the '[[clang::lifetime_non_invalidating]]' promise on parameter 'v'}}
  // expected-note@-5 {{overridden virtual function is here}}
  void touch(vector<int> &v) const override { v.push_back(42); }
};

// Repeating it is accepted, and then the body IS verified.
struct VHonest : VBase {
  // expected-warning@+1 {{this function invalidates parameter 'v', which its '[[clang::lifetime_non_invalidating]]' annotation promises not to invalidate}}
  void touch(vector<int> &v [[clang::lifetime_non_invalidating]]) const override {
    v.push_back(42); // expected-note {{invalidated here}}
  }
};

// A redeclaration needs no `override` to lose it. The attribute is an
// InheritableAttr rather than an InheritableParamAttr, so it is not propagated to
// a later declaration's parameter by default; without propagating it the promise
// on one declaration suppressed every call site while the DEFINITION's parameter
// never carried it, so the body was never verified.
void tickle(vector<int> &v [[clang::lifetime_non_invalidating]]);

// expected-warning@-2 {{this function invalidates parameter 'v', which its '[[clang::lifetime_non_invalidating]]' annotation promises not to invalidate}}
void tickle(vector<int> &v) {
  v.push_back(1); // expected-note {{invalidated here}}
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
