// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-dangling -verify %s

#include "Inputs/lifetime-analysis.h"

// A STATIC data member is a variable, not a subobject: `r.slot` and `R::slot`
// denote the very same object, and the object expression is evaluated and
// discarded. Only the qualified spelling is a DeclRefExpr, so the member
// spelling used to build an origin disconnected from the variable -- a store
// through it landed on a throwaway expression origin and the global-escape fact
// keyed on the VarDecl was never emitted. Both spellings must behave alike.

struct R {
  static int *slot;
  static int plain;
};
int *R::slot; // expected-note 5 {{this static storage dangles}}
int R::plain;

R *getR();
R &refR();

//===----------------------------------------------------------------------===//
// The two spellings of the same store.
//===----------------------------------------------------------------------===//

void via_object() {
  R r;
  int x = 7;
  r.slot = &x; // expected-warning {{stack memory associated with local variable 'x' escapes to the static variable 'slot' which will dangle}}
}

void via_qualified_name() {
  int x = 7;
  R::slot = &x; // expected-warning {{stack memory associated with local variable 'x' escapes to the static variable 'slot' which will dangle}}
}

//===----------------------------------------------------------------------===//
// The object expression can be any shape; it is evaluated and discarded, so the
// store reaches the same variable through every one of them.
//===----------------------------------------------------------------------===//

void via_pointer() {
  int x = 7;
  getR()->slot = &x; // expected-warning {{stack memory associated with local variable 'x' escapes to the static variable 'slot' which will dangle}}
}

void via_reference() {
  int x = 7;
  refR().slot = &x; // expected-warning {{stack memory associated with local variable 'x' escapes to the static variable 'slot' which will dangle}}
}

void via_temporary() {
  int x = 7;
  R{}.slot = &x; // expected-warning {{stack memory associated with local variable 'x' escapes to the static variable 'slot' which will dangle}}
}

//===----------------------------------------------------------------------===//
// Reached through a base class, a class template, and holding a view rather
// than a raw pointer.
//===----------------------------------------------------------------------===//

struct Base {
  static const char *p;
};
const char *Base::p; // expected-note 2 {{this static storage dangles}}
struct Derived : Base {};

void via_derived() {
  Derived d;
  std::string s;
  d.p = s.c_str(); // expected-warning {{stack memory associated with local variable 's' escapes to the static variable 'p' which will dangle}}
}

void via_base_qualified() {
  Derived d;
  std::string s;
  d.Base::p = s.c_str(); // expected-warning {{stack memory associated with local variable 's' escapes to the static variable 'p' which will dangle}}
}

template <class T> struct Tpl {
  static const char *p; // expected-note {{this static storage dangles}}
};
template <class T> const char *Tpl<T>::p;

void via_template() {
  Tpl<int> t;
  std::string s;
  t.p = s.c_str(); // expected-warning {{stack memory associated with local variable 's' escapes to the static variable 'p' which will dangle}}
}

struct V {
  static std::string_view sv;
};
std::string_view V::sv; // expected-note {{this static storage dangles}}

void view_member() {
  V v;
  std::string s;
  v.sv = s; // expected-warning {{stack memory associated with local variable 's' escapes to the static variable 'sv' which will dangle}}
}

//===----------------------------------------------------------------------===//
// No borrow of a local escapes in any of these, so they must stay silent. In
// particular, reading a scalar static member through an object must not look
// like a use of storage that holds no loan (which would report a lost loan
// where the qualified spelling is correctly silent).
//===----------------------------------------------------------------------===//

int g_int;
volatile int sink;

void store_address_of_global() {
  R r;
  r.slot = &g_int; // no-warning
}

void read_scalar_member() {
  R r;
  sink = r.plain; // no-warning
}

void read_scalar_qualified() {
  sink = R::plain; // no-warning
}

void write_scalar_member() {
  R r;
  r.plain = 5; // no-warning
}

void borrow_stays_local() {
  int x = 7;
  int *p = &x;
  sink = *p; // no-warning
}
