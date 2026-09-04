// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string_view;

// Forwarding a [[clang::noescape]] parameter to a method declared
// [[clang::lifetime_capture_by(this)]] captures it into the object, which the
// promise forbids. The capture is routed by the loans the receiver's LVALUE
// holds, so it reaches the object whatever expression designated it -- an
// inherited method's receiver arrives as a derived-to-base conversion whose own
// origin is a disconnected copy.
//
// The store's destination was read from the state AFTER the capture's own flow
// had merged the payload into the destination origin. The payload's parameter was
// then sitting among the destination's loans, so the store looked like a
// self-store into that very parameter and the check stepped aside. Only the
// direct `this` receiver survived, and only because a second, narrower route
// (the CapturedByThis escape fact, which recognizes just that spelling) reported
// it. Every base-class spelling was silent.
//
// Emitting the store before the flow makes the pre-store state genuinely
// pre-store. A real self-store still has the parameter in its destination loans
// before the store, so it is still excluded.

volatile int sink;

//===----------------------------------------------------------------------===//
// Caught: the noescape borrow reaches the object however the receiver is spelled.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(int)]] Base {
  const int *d = nullptr; // expected-note {{escapes to this field}}
  void setD(const int *q [[clang::lifetime_capture_by(this)]]) { d = q; } // honest
};

struct [[gsl::Pointer(int)]] Own {
  const int *d = nullptr; // expected-note {{escapes to this field}}
  void setOwn(const int *q [[clang::lifetime_capture_by(this)]]) { d = q; }

  // A direct store into the object's own field.
  void a_direct(const int *q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    d = q;
  }
  // Through a helper of the same class, which was caught all along.
  void b_own_helper(const int *q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    setOwn(q); // expected-note {{param returned here}}
  }
};

struct [[gsl::Pointer(int)]] Derived : Base {
  // The reported shape: an INHERITED helper, whose receiver is an implicit
  // derived-to-base conversion.
  void c_inherited(const int *q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    setD(q); // expected-note {{escapes into an object the caller owns here}}
  }
  // Explicitly qualified, which is the same call.
  void d_qualified(const int *q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    Base::setD(q); // expected-note {{escapes into an object the caller owns here}}
  }
  // Through an explicit upcast. An AST-level peel of the IMPLICIT conversion
  // would not reach this one; following the receiver's loans does.
  void e_upcast(const int *q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    static_cast<Base *>(this)->setD(q); // expected-note {{escapes into an object the caller owns here}}
  }
  // A direct store into the inherited field, for contrast.
  void f_direct_base_field(const int *q [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    d = q;
  }
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// Capturing into a LOCAL object is not an escape out of this function. (The
// object is seeded with an immortal borrow so the lost-borrow sentinel, which
// fires for an object holding no loan at all, has nothing to say here.)
static const int kImmortal = 3;

struct [[gsl::Pointer(int)]] Seeded {
  const int *d = nullptr;
  explicit Seeded(const int *init [[clang::lifetimebound]]) : d(init) {}
  void setD(const int *q [[clang::lifetime_capture_by(this)]]) { d = q; }
};

void into_local(const int *q [[clang::noescape]]) {
  Seeded o(&kImmortal);
  o.setD(q); // no-warning
  sink = *o.d;
}

// A capture that is declared rather than forbidden.
struct [[gsl::Pointer(int)]] Declared {
  const int *d = nullptr;
  void setD(const int *q [[clang::lifetime_capture_by(this)]]) { d = q; }
  void fwd(const int *q [[clang::lifetime_capture_by(this)]]) {
    setD(q); // no-warning: the capture is declared
  }
};
