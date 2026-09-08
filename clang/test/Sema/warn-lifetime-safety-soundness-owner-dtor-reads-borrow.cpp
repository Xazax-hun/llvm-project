// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// Locals are destroyed in reverse declaration order, so an object declared FIRST is
// destroyed LAST -- after everything it was given a borrow of. A destructor that
// reads such a borrow is therefore reading freed storage, and the analysis models a
// non-trivial destructor as a USE of the object precisely so that the borrowed-from
// local is reported at its own expiry.
//
// Owners were excluded from that, on the grounds that destroying an owner frees what
// it owns rather than dereferencing a borrow into something else. True of the owner's
// own storage -- but an owner can ALSO hold a borrow into something it does not own,
// and since a user owner's members became tracked, that borrow is visible. So a guard
// whose destructor reads a view member missed the reverse-order bug, for exactly the
// types most likely to have a logging destructor.
//
// The line is whether the owner is LIBRARY-owned. A user owner's members are tracked
// and its own destructor can read them. A library owner's are opaque: destroying a
// std::unique_ptr destroys its pointee, and whether THAT reads a borrow is the
// pointee's own destructor's business, modelled where the pointee is destroyed.

volatile char sink;

//===----------------------------------------------------------------------===//
// Caught: the guard outlives what its destructor reads.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(char)]] Span {
  ~Span() { emit(); }
  void emit() const { sink = name_.data()[0]; }
  friend void reversed_order();
  friend void correct_order();

private:
  string_view name_;
};

// The reported shape: the guard is declared first, so it is destroyed last.
void reversed_order() {
  Span s;
  string op("operation-name");
  s.name_ = op; // expected-warning {{local variable 'op' does not live long enough}}
} // expected-note {{destroyed here}} expected-note {{later used here}}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// Declared in the safe order: the guard dies before what it borrows.
void correct_order() {
  string op("operation-name");
  Span s;
  s.name_ = op; // no-warning
}

// A trivial destructor cannot read the borrow, so its destruction is no use.
struct [[gsl::Owner(char)]] PlainSpan {
  friend void trivial_dtor();

private:
  string_view name_;
};

void trivial_dtor() {
  PlainSpan s;
  string op("operation-name");
  s.name_ = op; // no-warning
}

// A LIBRARY owner's members are opaque; its own destructor reads none of them, and
// what its pointee's destructor does is modelled where the pointee dies. Declaring
// one before a local it transitively borrows must not report here.
struct Borrower {
  Borrower(const string &s [[clang::lifetimebound]]);
};

void library_owner_first() {
  std::unique_ptr<Borrower> p;
  string op("operation-name");
  // (The unannotated unique_ptr constructor draws its own annotation demand; the
  // point here is that no destruction-order report fires.)
  // expected-warning@+1 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
  p = std::unique_ptr<Borrower>(new Borrower(op));
  (void)p;
}
