// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;

// For an ARROW member store the container is what the base points AT: `n->d`
// stores into `*n`, not into the pointer variable `n`. A pointer VARIABLE has
// storage of its own, so its lvalue origin carries a loan naming that variable
// while the object lives on the pointee origin -- hence the descent.
//
// Only an LVALUE base is a pointer object like that. A prvalue pointer
// expression has no storage, so its own origin already denotes what it points
// at, and descending goes a level too deep and loses the object: a store through
// `(&c)->d` targeted nothing and went unreported, while `c.d`, `(*&c).d` and
// `Cache *lp = &c; lp->d` -- the same store, differently spelled -- were caught.
//
// `this` is the same shape, and used to be the one prvalue excluded by name.
// Asking the value category covers it and every other prvalue receiver.

volatile int sink;

//===----------------------------------------------------------------------===//
// The same store must be reported however the receiver is spelled.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(int)]] Cache {
  friend void via_dot(Cache &c [[clang::noescape]]);
  friend void via_addressof_arrow(Cache &c [[clang::noescape]]);
  friend void via_pointer_variable(Cache &c [[clang::noescape]]);
  friend void via_deref_addressof(Cache &c [[clang::noescape]]);
  friend void via_call_returning_pointer(Cache &c [[clang::noescape]]);
  friend void from_static(Cache &c [[clang::noescape]]);
  friend Cache *identity(Cache *c [[clang::lifetimebound]]);

private:
  const int *current = nullptr; // expected-note 5 {{this field dangles}}
};

Cache *identity(Cache *c [[clang::lifetimebound]]) { return c; }

void via_dot(Cache &c [[clang::noescape]]) {
  int v = 1;
  c.current = &v; // expected-warning {{stack memory associated with local variable 'v' escapes to the field 'current'}}
}

// The reported shape: a prvalue `&c` as the arrow base.
void via_addressof_arrow(Cache &c [[clang::noescape]]) {
  int v = 1;
  (&c)->current = &v; // expected-warning {{stack memory associated with local variable 'v' escapes to the field 'current'}}
}

// An lvalue pointer variable, which needs the descent and must keep it.
void via_pointer_variable(Cache &c [[clang::noescape]]) {
  int v = 1;
  Cache *lp = &c;
  lp->current = &v; // expected-warning {{stack memory associated with local variable 'v' escapes to the field 'current'}}
}

// The dot spelling of the same thing.
void via_deref_addressof(Cache &c [[clang::noescape]]) {
  int v = 1;
  (*&c).current = &v; // expected-warning {{stack memory associated with local variable 'v' escapes to the field 'current'}}
}

// Another prvalue receiver: the result of a call.
void via_call_returning_pointer(Cache &c [[clang::noescape]]) {
  int v = 1;
  identity(&c)->current = &v; // expected-warning {{stack memory associated with local variable 'v' escapes to the field 'current'}}
}

// `this` is a prvalue too, and was the one spelling excluded by name. Storing a
// noescape parameter's borrow through it must stay reported.
struct [[gsl::Owner(int)]] SelfStore {
  void set(const int *p [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    this->current = p;
  }
  void set_implicit(const int *p [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    current = p;
  }

private:
  const int *current = nullptr; // expected-note 2 {{escapes to this field}}
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A store into a LOCAL object is no escape: that object dies before the source.
struct [[gsl::Owner(int)]] LocalDest {
  friend void into_local();
  int read() const { return *current; }

private:
  const int *current = nullptr;
};

void into_local() {
  int v = 1;
  LocalDest d;
  (&d)->current = &v; // no-warning: `d` dies before `v` does
  sink = d.read();
}

// A source that outlives every call.
static const int kImmortal = 5;

void from_static(Cache &c [[clang::noescape]]) {
  (&c)->current = &kImmortal; // no-warning
}
