// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-capture-by-violation -Wno-dangling -Wno-dangling-gsl -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// '[[clang::lifetimebound]]' describes the RETURN VALUE and nothing else. A method that also
// stores the parameter's borrow into a member establishes a second relationship the
// declaration does not advertise: the object now aliases the argument, and a caller reading
// only the declaration cannot tell it must keep the argument alive.
//
// Nothing checked that. The lifetimebound claim itself is satisfied -- the function really
// does return the parameter -- and the annotation suppressed the unannotated-indirection
// backstop, so the capture went unchecked and the borrow could dangle with no diagnostic
// anywhere.
//
// '[[clang::lifetime_capture_by(this)]]' is the annotation for it. Once written, the capture
// is modeled and a dangling use is reported at the CALL SITE, which is where the caller can
// act on it.

volatile char sink;

class [[gsl::Owner]] StoresIntoOwner {
  string_view key;

public:
  // expected-warning@+1 {{describes the RETURN VALUE, but the borrow from 'k' is also captured into this object}}
  string_view rekey(const string &k [[clang::lifetimebound]]) {
    key = k;
    return k;
  }
};

// The member need not be a view: a raw pointer into the argument is the same capture.
class [[gsl::Owner]] StoresPointer {
  const char *key = nullptr;

public:
  // expected-warning@+1 {{describes the RETURN VALUE, but the borrow from 'k' is also captured into this object}}
  const char *rekey(const string &k [[clang::lifetimebound]]) {
    key = k.data();
    return key;
  }
};

// A view type is the same: what makes this wrong is the undeclared capture, not the class's
// ownership annotation.
class [[gsl::Pointer]] StoresIntoView {
  string_view key;

public:
  // expected-warning@+1 {{describes the RETURN VALUE, but the borrow from 'k' is also captured into this object}}
  string_view rekey(const string &k [[clang::lifetimebound]]) {
    key = k;
    return k;
  }
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// Declaring the capture is the point of the rule, so a parameter that names `this` is
// excluded -- it is validated elsewhere.
class [[gsl::Pointer]] DeclaresTheCapture {
  string_view key;

public:
  void rekey(const string &k [[clang::lifetime_capture_by(this)]]) { key = k; } // no-warning
};

// Returning the borrow WITHOUT storing it is exactly what lifetimebound describes.
class [[gsl::Owner]] OnlyReturns {
  string owned;

public:
  string_view peek(const string &k [[clang::lifetimebound]]) { return k; } // no-warning
};

// Storing an owned COPY captures nothing.
class [[gsl::Owner]] StoresACopy {
  string owned;

public:
  string_view rekey(const string &k [[clang::lifetimebound]]) { // no-warning
    owned = k;
    return k;
  }
};

// A store into a member from something other than the annotated parameter.
class [[gsl::Pointer]] StoresSomethingElse {
  string_view key;

public:
  string_view rekey(const string &k [[clang::lifetimebound]]) { // no-warning
    key = string_view();
    return k;
  }
};
