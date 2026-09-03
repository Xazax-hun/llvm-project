// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s
#include "Inputs/lifetime-analysis.h"

using std::string_view;

// A [[clang::lifetime_capture_by(X)]] parameter promises its borrow is captured
// by X. If the body instead captures it into the enclosing object (a store into
// a field of `this`) while X does not name `this`, the annotation is a lie: it
// suppresses the unannotated-indirection backstop, so the real capture into the
// object goes unchecked and the borrow can dangle once the object outlives it.
// The body must be verified against the annotation.

struct [[gsl::Owner(int)]] Box {
  Box();
  // Lying: names by-value param 'decoy' as the capturer, but stores the borrow
  // into *this.
  // 'decoy' is itself an owner, so naming it is refused too (owner-capture) --
  // both findings are real and independent.
  // `decoy` can hold a borrow now that an owner's members are tracked, so the
  // ordinary annotation demand applies to it too.
  void bad(Box decoy, string_view s [[clang::lifetime_capture_by(decoy)]]) { // expected-warning {{names a capturing entity other than 'this'}} expected-warning {{is meant to own its contents}} expected-warning {{parameter that can hold a borrow is not annotated}}
    p = s.data();
  }

  // Truthful capture_by(this) is validated elsewhere (owner-capture) and must
  // NOT be additionally reported as a capture_by violation.
  void cap_this(string_view s [[clang::lifetime_capture_by(this)]]) { // expected-warning {{lifetime_capture_by(this)}}
    p = s.data();
  }

private:
  const char *p = nullptr;
};
