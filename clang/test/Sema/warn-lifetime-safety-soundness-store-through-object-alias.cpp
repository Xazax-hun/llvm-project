// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;

// Binding an alias to an object gives the alias a COPY of the object's origin
// tree: loans flow in at the binding, and nothing flows back out. A store through
// the alias therefore deposited the borrow in the copy while every check read the
// object's own origin, so `Box &s = *this; s.d = local` was silent while
// `d = local` one line away was reported. The same held for every other way of
// naming the object -- a pointer to it, a dereference of one, a conditional
// between two, a [[clang::lifetimebound]] accessor returning it.
//
// A store is now also ROUTED by the loans its own member lvalue holds. Those name
// the storage written -- the object's loan projected by the field -- so they reach
// the object's field origin whatever expression designated the object. One rule
// covers every spelling, where matching them in the AST would be an open-ended
// list. No flow back out of the alias is needed, and none would work: loan
// propagation is a forward dataflow, so a back-edge at the binding carries
// nothing, the borrow not existing yet at that point.
//
// A field of the implicit object has two origins -- the per-FieldDecl one, which a
// read and a direct `d = ...` store consult, and the structural child of the
// `this` origin list. Resolution lands on the former, which is what its contract
// asks for ("the origin a read of the same member consults"). Landing on the
// latter would deposit the borrow where no read looks and, because that child is
// part of the implicit `this` use at exit, would make it live at the source's
// expiry -- turning the field-specific report into a coarser use-after-scope one.

volatile char sink;

//===----------------------------------------------------------------------===//
// The same store, however the object is named.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(char)]] Box {
  char read() const { return d[0]; }
  Box *self() [[clang::lifetimebound]] { return this; }

  // Directly on `this`, which was reported all along.
  void direct() {
    string l("aaaa");
    d = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' escapes to the field 'd'}}
  }
  // The reported shape: a reference bound to `*this`.
  void via_reference() {
    string l("aaaa");
    Box &s = *this;
    s.d = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' escapes to the field 'd'}}
  }
  // A pointer to the object.
  void via_pointer() {
    string l("aaaa");
    Box *p = this;
    p->d = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' escapes to the field 'd'}}
  }
  // The dot spelling through that pointer.
  void via_deref_pointer() {
    string l("aaaa");
    Box *p = this;
    (*p).d = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' escapes to the field 'd'}}
  }
  // A conditional between two names for the same object.
  void via_conditional(bool c) {
    string l("aaaa");
    Box *p = c ? this : this;
    p->d = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' escapes to the field 'd'}}
  }
  // Through a [[clang::lifetimebound]] accessor returning the object. Reported
  // through the expiry rather than as a field escape, but reported.
  void via_accessor() {
    string l("aaaa");
    self()->d = l.c_str(); // expected-warning {{local variable 'l' does not live long enough}}
    // expected-note@+2 {{destroyed here}}
    // expected-note@+1 {{later used here}}
  }

private:
  const char *d = ""; // expected-note 5 {{this field dangles}}
};

// The object need not be `this`: a pointer to a LOCAL object reaches it the same
// way, which was a documented false negative ("requires alias analysis").
struct [[gsl::Owner(char)]] Local {
  friend Local escapes_via_pointer_to_local();
  char read() const { return d[0]; }

private:
  const char *d = "";
};

Local escapes_via_pointer_to_local() {
  Local s;
  string l("aaaa");
  Local *p = &s;
  p->d = l.c_str(); // expected-warning {{stack memory associated with local variable 'l' is returned}}
  return s;         // expected-note {{returned here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(char)]] Quiet {
  friend void from_immortal_via_alias();
  friend void into_local_object_via_alias();
  char read() const { return d[0]; }

private:
  const char *d = "";
};

// A source that outlives everything, stored through an alias.
static const char kImmortal[] = "immortal";

void from_immortal_via_alias() {
  Quiet q;
  Quiet &s = q;
  s.d = kImmortal; // no-warning
  sink = q.read();
}

// A store into a local object that dies before the source: no escape, and the
// object's own expiry is what would check it.
void into_local_object_via_alias() {
  string l("aaaa");
  {
    Quiet q;
    Quiet &s = q;
    s.d = l.c_str(); // no-warning: `q` dies first
    sink = q.read();
  }
}
