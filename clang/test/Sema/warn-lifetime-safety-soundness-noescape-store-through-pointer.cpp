// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A [[clang::noescape]] borrow stored into another parameter's object escapes, and
// that is checked by looking for a parameter PLACEHOLDER among the loans the
// store's container holds.
//
// Through a reference it worked: a reference has no storage of its own, so its
// lvalue origin IS the referred-to object's, and the placeholder is right there.
// Through a POINTER it did not: a pointer variable has storage, so its lvalue
// origin carries a loan naming that variable, while the caller's object lives on
// the POINTEE origin. The store therefore looked like it targeted the local
// pointer, and a noescape borrow written through `n->d` escaped unreported --
// while `n.d` one line away was caught.
//
// An arrow access stores into what the base points at, so the container is now
// the pointee. `this` is excluded: the model already gives it the object's
// origin, not a pointer's.

volatile char sink;

//===----------------------------------------------------------------------===//
// The escape must be reported whichever way the destination is spelled.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(char)]] NodePtr {
  friend void fillPtr(NodePtr *n [[clang::noescape]],
                      string_view v [[clang::noescape]]);

private:
  const char *d = nullptr;
};
// The reported shape: destination reached through a POINTER parameter.
void fillPtr(NodePtr *n [[clang::noescape]],
             string_view v [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  n->d = v.data(); // expected-note {{escapes into an object the caller owns here}}
}

struct [[gsl::Owner(char)]] NodeRef {
  friend void fillRef(NodeRef &n [[clang::noescape]],
                      string_view v [[clang::noescape]]);

private:
  const char *d = nullptr;
};
// Through a reference, which was caught all along -- the two must agree.
void fillRef(NodeRef &n [[clang::noescape]],
             string_view v [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  n.d = v.data(); // expected-note {{escapes into an object the caller owns here}}
}

// Storing the VIEW itself, rather than a pointer obtained from it.
struct [[gsl::Owner(char)]] NodeView {
  friend void fillView(NodeView *n [[clang::noescape]],
                       string_view v [[clang::noescape]]);

private:
  string_view d;
};
void fillView(NodeView *n [[clang::noescape]],
              string_view v [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  n->d = v; // expected-note {{escapes into an object the caller owns here}}
}

// The stored borrow coming from a raw pointer parameter rather than a view.
struct [[gsl::Owner(char)]] NodeRaw {
  friend void fillRaw(NodeRaw *n [[clang::noescape]],
                      const char *p [[clang::noescape]]);
  friend void unrelated_source(NodeRaw *n [[clang::noescape]],
                               string_view v [[clang::noescape]]);

private:
  const char *d = nullptr;
};
void fillRaw(NodeRaw *n [[clang::noescape]], const char *p [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  n->d = p; // expected-note {{escapes into an object the caller owns here}}
}

// Into `this`, which the exclusion must leave working.
struct [[gsl::Owner(char)]] Self {
  void set(string_view v [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    d = v.data();
  }

private:
  const char *d = nullptr; // expected-note {{escapes to this field}}
};

// Spelled explicitly through `this->`, which is the arrow form on `this`.
struct [[gsl::Owner(char)]] SelfArrow {
  void set(string_view v [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    this->d = v.data();
  }

private:
  const char *d = nullptr; // expected-note {{escapes to this field}}
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// The stored value is not a borrow of the noescape parameter, so nothing escapes.
void unrelated_source(NodeRaw *n [[clang::noescape]],
                      string_view v [[clang::noescape]]) {
  n->d = "immortal string literal"; // no-warning
  (void)v;
}
