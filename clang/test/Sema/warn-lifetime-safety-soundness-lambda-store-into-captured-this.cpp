// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A lambda body is a separate function. Nothing it writes is flowed back to the
// enclosing function -- the closure gets a single merged origin that tracks only
// whether the lambda outlives a capture -- and the body's own analysis cannot see
// the lifetimes of the enclosing locals it captured, since from inside they are
// just variables of some other function.
//
// So a `[this]` capture whose body stores a borrow into a member of the enclosing
// object is invisible from both sides: the enclosing function sees a closure being
// made and called, and the lambda sees a store of a borrow whose owner it believes
// outlives everything. A borrow of an enclosing local left in the object dangles
// with nothing reported.
//
// This is the same "not flowed back" gap the by-reference indirection capture is
// already refused for, reached through the captured object instead of through the
// captured variable. Refuse it rather than model it.
//
// The question is asked of the LOANS, not of the store's syntax: inside a lambda,
// a borrow rooted at a variable of an ENCLOSING function is necessarily a capture,
// and this CFG holds no Expire for it, so it looks immortal here. Escaping into
// the object is then the hazard, however the store was written. Matching the store
// in the AST instead would need a new case per spelling -- an overloaded
// operator=, a helper called on the object, a whole-object assignment, an alias.

volatile char sink;

//===----------------------------------------------------------------------===//
// Refused: the body stores a borrow into the captured enclosing object.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] Holder {
  string_view v;

  // The reported shape: an immediately-invoked lambda storing a view of a local.
  void go() {
    string local("aaaa");
    [this, &local] { v = string_view(local); }(); // expected-warning {{storing a borrow into a member of the captured enclosing object is not modeled}}
  }

  // Not invoked here: the closure could be called anywhere, which is no better.
  void store_for_later() {
    string local("aaaa");
    auto f = [this, &local] { v = string_view(local); }; // expected-warning {{storing a borrow into a member of the captured enclosing object is not modeled}}
    f();
  }

  // `this` captured implicitly by a default capture.
  void implicit_capture() {
    string local("aaaa");
    [&] { v = string_view(local); }(); // expected-warning {{storing a borrow into a member of the captured enclosing object is not modeled}}
  }

  // Spelled through `this->`, which is the same store.
  void explicit_this() {
    string local("aaaa");
    [this, &local] { this->v = string_view(local); }(); // expected-warning {{storing a borrow into a member of the captured enclosing object is not modeled}}
  }
};

// A raw pointer member, where the assignment is a builtin operator rather than an
// overloaded one -- both spellings have to be recognized.
struct [[gsl::Pointer(char)]] RawHolder {
  const char *p = nullptr;

  void go() {
    string local("aaaa");
    [this, &local] { p = local.c_str(); }(); // expected-warning {{storing a borrow into a member of the captured enclosing object is not modeled}}
  }
};

// Other ways the borrow can reach the object. These are refused too -- by this
// check or by another -- and the point of asking the loans is that none of them
// needs its own case here.
struct [[gsl::Pointer(char)]] OtherSpellings {
  string_view v;
  void setV(string_view s [[clang::lifetime_capture_by(this)]]) { v = s; }

  // Through a helper called on the object rather than a direct store.
  void via_helper() {
    string local("aaaa");
    // expected-warning@+2 {{lifetime safety cannot track this value here}}
    // expected-warning@+1 {{assignment through this expression is not modeled}}
    [this, &local] { setV(string_view(local)); }();
  }

  // A whole-object assignment, which names no member at all.
  void via_whole_object() {
    string local("aaaa");
    // expected-warning@+2 {{lifetime safety cannot track this value here}}
    // expected-warning@+1 {{assignment through this expression is not modeled}}
    [this, &local] { *this = OtherSpellings{string_view(local)}; }();
  }
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

struct Quiet {
  string_view v;
  int n = 0;
  const char *p = nullptr;

  // Storing into a member that cannot hold a borrow is not this hazard.
  void scalar_member() {
    int local = 1;
    [this, local] { n = local; }();
  }

  // Only reading a member.
  void reads_only() {
    [this] { sink = v.data()[0]; }();
  }

  // Reading the captured local, rather than letting a borrow of it escape.
  void reads_capture() {
    string local("aaaa");
    [this, &local] { sink = local.c_str()[0]; }();
  }

  // Storing a borrow of something immortal is no hazard, even into the object.
  void stores_immortal() {
    [this] { v = string_view("immortal literal"); }();
  }

  // No `this` capture at all: the lambda cannot reach a member. (The by-ref
  // capture of a view draws the pre-existing indirection refusal, and the
  // uninitialized view the lost-borrow sentinel; neither is this check.)
  void no_this_capture() {
    string_view local_view;
    string local("aaaa");
    // expected-warning@+2 {{lifetime safety cannot track local variable 'local_view' here}}
    // expected-warning@+1 {{by-reference capture of 'local_view' uses more than one level of indirection}}
    [&local_view, &local] { local_view = string_view(local); }();
  }

  // A store into a LOCAL object of the same shape, not into the captured object.
  // (`Quiet` is annotated neither Owner nor Pointer, so it draws the pre-existing
  // unknown-ownership refusal; again, not this check.)
  void into_local_object() {
    string local("aaaa");
    Quiet other; // expected-warning {{type 'Quiet' can hold a borrow but is annotated neither}}
    [&other, &local] { other.v = string_view(local); }();
    sink = other.v.data()[0]; // expected-warning {{lifetime safety cannot track local variable 'other' here}}
  }
};
