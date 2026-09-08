// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A borrow of the enclosing object or one of its members must not escape to global
// storage: intra-procedurally the object is caller-scope -- a placeholder that never
// expires -- so the store is otherwise accepted even though the global outlives the
// caller's object.
//
// A member's origin is seeded at entry with an Uninitialized loan naming that field.
// That IS the borrow the caller left in the member, and it is what a read of the
// member yields, but its path kind is Uninitialized rather than ValueDecl, so
// getAsValueDecl() returned null and the escape was classified as neither a `this`
// borrow nor a field borrow. Publishing a member's borrow to a global was therefore
// silent, while the same store of a LOCAL's borrow one function over was reported.
//
// Not about constness, and not about which member is read: the non-const spelling
// and a whole-object store were equally silent.

volatile char sink;

static const char kImmortal[] = "immortal";
string_view g = string_view(kImmortal); // expected-note {{this global dangles}}

//===----------------------------------------------------------------------===//
// Caught: a borrow held by the object reaches a global.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] Holder {
  string_view q;

  // The reported shape: a const method publishing a member's borrow.
  void publish_const() const { g = q; } // expected-warning {{a borrow of the enclosing object or one of its members escapes to global}}

  // Constness is not what made it silent.
  void publish_nonconst() { g = q; } // expected-warning {{a borrow of the enclosing object or one of its members escapes to global}}

  // Storing a copy of the member rather than the member itself.
  void publish_copy() const { g = string_view(q); } // expected-warning {{a borrow of the enclosing object or one of its members escapes to global}}
};

// A borrow of a LOCAL escaping to a global, which was reported all along -- the two
// must agree.
void publish_local() {
  string s("xxxx");
  g = string_view(s); // expected-warning {{stack memory associated with local variable 's' escapes to the global}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] Quiet {
  string_view q;

  // Reading a member without publishing it.
  char peek() const { return q.data()[0]; }

  // Storing something IMMORTAL into the global is no escape of the object.
  void publish_immortal() const { g = string_view(kImmortal); } // no-warning

  // Storing into a member of the object, not into a global.
  void keep(string_view in [[clang::lifetime_capture_by(this)]]) { q = in; } // no-warning
};

// A global assigned from another global outlives nothing. (The source global draws
// the lost-borrow sentinel, since nothing in this function flows a borrow into it;
// that is not this check.)
string_view g2 = string_view(kImmortal);

void global_to_global() {
  g = g2; // expected-warning {{lifetime safety cannot track global variable 'g2' here}}
}
