// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// '[[clang::lifetimebound]]' describes the RETURN VALUE and nothing else. Storing
// the parameter's borrow into an object the CALLER owns is a second, undeclared
// relationship: that object now aliases the argument, and a caller reading only
// the declaration cannot tell it must keep the argument alive. The annotation also
// suppresses the unannotated-indirection backstop, so the capture went unchecked
// while the lifetimebound claim itself stayed truthful -- the function really does
// return the parameter.
//
// That was asked for the implicit object, driven by the escape facts. A parameter's
// object has no escape fact -- the store is all there is to see -- so the identical
// capture one parameter over went unreported.
//
// The other annotations on the source were already answered: noescape forbids the
// store outright, and an unannotated parameter is demanded to be annotated. Only
// lifetimebound slipped between them.

volatile char sink;

//===----------------------------------------------------------------------===//
// Caught: a lifetimebound borrow captured into a caller-owned object.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(char)]] Cache {
  friend const char *into_param(Cache &c [[clang::noescape]],
                                const char *p [[clang::lifetimebound]]);
  friend const char *into_param_arrow(Cache *c [[clang::noescape]],
                                      const char *p [[clang::lifetimebound]]);
  friend const char *into_by_value(Cache c [[clang::noescape]],
                                   const char *p [[clang::lifetimebound]]);
  friend void from_noescape(Cache &c [[clang::noescape]],
                            const char *p [[clang::noescape]]);

private:
  const char *d_ = "";
};

// The reported shape: the destination is a reference parameter, so the caller owns
// the object the borrow lands in.
const char *into_param(Cache &c [[clang::noescape]],
                       const char *p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE, but the borrow from 'p' is also captured into 'c', which the caller owns}}
  c.d_ = p;
  return p;
}

// Through a pointer parameter, which is the same object.
const char *into_param_arrow(Cache *c [[clang::noescape]],
                             const char *p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE, but the borrow from 'p' is also captured into 'c', which the caller owns}}
  c->d_ = p;
  return p;
}

// Into the implicit object, which was reported all along -- the two must agree,
// and the wording differs only in which object is named.
struct [[gsl::Owner(char)]] SelfCache {
  const char *into_this(const char *p [[clang::lifetimebound]]) { // expected-warning {{describes the RETURN VALUE, but the borrow from 'p' is also captured into this object}}
    d_ = p;
    return p;
  }

private:
  const char *d_ = "";
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A BY-VALUE destination is the callee's own copy, which dies with the call, so
// depositing a borrow in it captures nothing the caller can observe.
const char *into_by_value(Cache c [[clang::noescape]],
                          const char *p [[clang::lifetimebound]]) {
  c.d_ = p; // no-warning
  return p;
}

// A noescape source is forbidden from escaping at all, and its own check says so
// in its own wording; this one must not also fire.
void from_noescape(Cache &c [[clang::noescape]],
                   const char *p [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  c.d_ = p; // expected-note {{escapes into an object the caller owns here}}
}

// The capture is DECLARED. Which entity the annotation names is the capture_by
// checks' question, so this one steps aside rather than calling a declared
// relationship undeclared.
struct [[gsl::Pointer]] View {
  const char *q = "";
};

// It also draws the unrelated suggestion to mark the returned parameter
// lifetimebound, which is a different question from the capture.
const char *declared(View &v [[clang::noescape]], // expected-warning {{uses more than one level of indirection}}
                     const char *p [[clang::lifetime_capture_by(v)]]) { // expected-warning {{should be marked [[clang::lifetimebound]]}}
  v.q = p;      // no-warning from the undeclared-capture check
  return p;     // expected-note {{param returned here}}
}

// Storing a borrow of the destination into itself is self-reference, not the
// capture of a second object.
struct [[gsl::Owner(char)]] SelfStore {
  friend void self_store(SelfStore &c [[clang::noescape]]);

private:
  const char *d_ = "";
  char buf_[4] = {};
};

void self_store(SelfStore &c [[clang::noescape]]) {
  c.d_ = c.buf_; // expected-warning {{member is bound to a sibling member of the same object}}
}
