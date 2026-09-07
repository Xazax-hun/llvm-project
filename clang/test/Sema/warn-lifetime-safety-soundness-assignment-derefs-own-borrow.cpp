// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;

// An assignment is modeled as kill-then-propagate, which assumes the operator only
// RESEATS the borrow. An assignment operator whose body instead reads or writes
// THROUGH the borrow the object already holds does more than that, and the model
// discards exactly the part that can dangle: it kills the loan the dereference
// reads, and records a WRITE of the object where the body performs a READ of it.
//
// So an invalidation before `r = 'x'` went unreported, while the same access
// spelled as an ordinary method call (`r.set('x')`) was caught -- the two differ
// only in which member function is named.
//
// The question is asked of the LOANS, not of the body's syntax. A member's origin
// is seeded at entry with an Uninitialized loan naming that field, so a USE of an
// origin carrying such a loan is precisely "this operator reads the borrow the
// object already holds", however the dereference is written. A pure reseat never
// uses one: it reads the RIGHT operand's borrow, whose loans are rooted at that
// parameter instead. Comparing or null-checking the old pointer is not a use of
// what it points at, and stays silent.
//
// Residual, and inherent to an intra-procedural analysis: if the dereference lives
// in a HELPER the operator calls, it is in another function's body and is not seen
// here.

volatile char sink;

//===----------------------------------------------------------------------===//
// Refused: the operator reads through the borrow the object already holds.
//===----------------------------------------------------------------------===//

// The reported shape: the right operand carries no borrow at all, so the operator
// cannot be reseating anything -- it writes through what is already there.
struct [[gsl::Pointer(char)]] WriteThrough {
  char *p = nullptr;
  explicit WriteThrough(char *q [[clang::lifetimebound]]) : p(q) {}
  void operator=(char c) {
    if (p)
      *p = c; // expected-warning {{assignment operator reads or writes through a borrow the object already holds}}
  }
};

// Reads through it rather than writing.
struct [[gsl::Pointer(char)]] ReadThrough {
  char *p = nullptr;
  explicit ReadThrough(char *q [[clang::lifetimebound]]) : p(q) {}
  void operator=(char) {
    sink = *p; // expected-warning {{assignment operator reads or writes through a borrow the object already holds}}
  }
};

// Subscript, which is the same dereference differently spelled -- and needs no
// case of its own, because the loan is what is being asked about.
struct [[gsl::Pointer(char)]] Subscript {
  char *p = nullptr;
  explicit Subscript(char *q [[clang::lifetimebound]]) : p(q) {}
  // A subscript use carries no expression to anchor at, so the report falls back
  // to the operator itself.
  void operator=(char c) { // expected-warning {{assignment operator reads or writes through a borrow the object already holds}}
    p[0] = c;
  }
};

// The right operand DOES carry a borrow, so this looks like a reseat -- but the
// body dereferences the old borrow instead of replacing it. Keying on the right
// operand's type alone would miss this.
struct [[gsl::Pointer(char)]] MixedReseat {
  char *p = nullptr;
  explicit MixedReseat(char *q [[clang::lifetimebound]]) : p(q) {}
  void operator=(MixedReseat o) { // expected-warning {{not annotated for lifetime safety}}
    if (p && o.p)
      *p = *o.p; // expected-warning {{assignment operator reads or writes through a borrow the object already holds}}
  }
};

//===----------------------------------------------------------------------===//
// Must stay silent: a pure reseat, which the assignment model does describe.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Reseat {
  char *p = nullptr;
  explicit Reseat(char *q [[clang::lifetimebound]]) : p(q) {}
  // Reads the RIGHT operand's borrow, never its own.
  // expected-warning@+1 {{not annotated for lifetime safety}}
  void operator=(Reseat o) { p = o.p; } // no dereference of its own borrow
};

struct [[gsl::Pointer(char)]] ReseatCompares {
  char *p = nullptr;
  explicit ReseatCompares(char *q [[clang::lifetimebound]]) : p(q) {}
  // Comparing the old pointer is not a use of what it points at.
  void operator=(ReseatCompares o) { // expected-warning {{not annotated for lifetime safety}}
    if (p != o.p)
      p = o.p;
  }
};

struct [[gsl::Pointer(char)]] ReseatNullChecks {
  char *p = nullptr;
  explicit ReseatNullChecks(char *q [[clang::lifetimebound]]) : p(q) {}
  void operator=(ReseatNullChecks o) { // expected-warning {{not annotated for lifetime safety}}
    if (!p)
      p = o.p;
  }
};

//===----------------------------------------------------------------------===//
// A reseat must still both PROPAGATE and KILL, or the refusal has broken the
// model it was protecting.
//===----------------------------------------------------------------------===//

static char kImmortal[8];

void reseat_propagates() {
  Reseat v(kImmortal);
  {
    char shortlived[8];
    v = Reseat(shortlived); // expected-warning {{'shortlived' does not live long enough}}
  }                         // expected-note {{destroyed here}}
  sink = *v.p;              // expected-note {{later used here}}
}

void reseat_kills() {
  Reseat v(kImmortal);
  {
    char shortlived[8];
    v = Reseat(shortlived);
    v = Reseat(kImmortal); // replaces the short-lived borrow
  }
  sink = *v.p; // no-warning: the loan that dangles was killed
}
