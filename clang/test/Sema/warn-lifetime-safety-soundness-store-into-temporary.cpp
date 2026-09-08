// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// Two things had to be right for a borrow stored into a TEMPORARY to be tracked.
//
// 1. A temporary is storage like any other and has an origin node, but
//    getOriginForAccessPath did not resolve one, so the store routed nowhere. That
//    was thought harmless because a non-extended temporary dies at the end of the
//    full expression, so nothing could read the borrow afterwards -- except its own
//    DESTRUCTOR, which runs at that very cleanup and is already modelled as a use of
//    the object.
//
// 2. Temporaries are destroyed in reverse order of CONSTRUCTION, and a destructor
//    only sees a borrow as dangling once the borrowed-from temporary's expiry has
//    been emitted. The CFG's list is in syntactic pre-order -- neither construction
//    nor destruction order -- so whether the temporary destroyed LAST came last was
//    accidental.
//
// Both are needed, and the second is what keeps the first from inventing reports:
// the two spellings below differ in SEQUENCING, and so in whether there is a bug at
// all. `a = b` sequences the right operand first, so the receiver temporary is
// constructed last and destroyed FIRST -- its destructor reads the borrow while the
// borrowed-from string is still alive, which is safe. `a.operator=(b)` sequences the
// object expression first, so the receiver is destroyed LAST and its destructor reads
// freed storage. Both are well defined; only the second is a bug.

volatile char sink;

struct [[gsl::Owner(char)]] W {
  ~W() { sink = v.data()[0]; }
  W() = default;
  friend void receiver_destroyed_last();
  friend void receiver_destroyed_first();
  friend void extended_temporary();

private:
  string_view v;
};

//===----------------------------------------------------------------------===//
// Reported: the receiver temporary is destroyed last.
//===----------------------------------------------------------------------===//

// The reported shape. `.operator=` sequences `W()` first, so ~W runs after the
// string temporary is gone.
void receiver_destroyed_last() {
  W().v.operator=(string("temporary")); // expected-warning {{local temporary object does not live long enough}}
  // expected-note@-1 {{destroyed here}} expected-note@-1 {{later used here}}
}

// An EXTENDED temporary outlives the full expression, so a borrow captured into it
// dangles at the source's scope exit. This used to draw only the coarse "assignment
// through this expression is not modeled" refusal.
void extended_temporary() {
  W &&w = W();
  {
    string s("temporary");
    w.v = s; // expected-warning {{local variable 's' does not live long enough}}
  } // expected-note {{destroyed here}}
  sink = w.v.data()[0]; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: the receiver temporary is destroyed FIRST.
//===----------------------------------------------------------------------===//

// Simple assignment sequences the right operand before the left, so `W()` is
// constructed last and destroyed first -- ~W reads the borrow while the string is
// still alive. Reporting this would be a false positive, and routing the store
// without fixing the destruction order produced exactly that.
void receiver_destroyed_first() {
  // (-Wdangling-assignment-gsl, an older and separate Sema check, does report here.
  // ASan confirms there is no bug: ~W runs before the string temporary is destroyed.
  // Not this analysis, and not in any lifetime-safety group.)
  // expected-warning@+1 {{object backing the pointer 'W().v' will be destroyed at the end of the full-expression}}
  W().v = string("temporary");
}
