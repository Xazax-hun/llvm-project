// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::vector;

// Aggregate initialization is modelled by zipping the initializers against
// `RD->fields()`, so that a reference member can be recognised and bound to the
// initializer's storage rather than to its value.
//
// An UNNAMED BIT-FIELD is in `fields()` but takes no initializer. The two lists then
// drift at the first one and every later initializer is attributed to the wrong
// member -- so a borrow bound to a reference member behind one was handled as an
// ordinary value and dropped. A NAMED bit-field does take an initializer, so it must
// still consume one; that spelling was correct all along, which is what pins this on
// the unnamed case rather than on bit-fields.

volatile int sink;

//===----------------------------------------------------------------------===//
// A [[clang::noescape]] argument bound to a reference member must be reported
// whatever precedes that member.
//===----------------------------------------------------------------------===//

// The reported shape: an unnamed bit-field between the members.
struct [[gsl::Pointer(int)]] Between {
  const int *p;
  int : 3;
  const int &r;
};

Between between(const int &x [[clang::lifetimebound]],
                const vector<int> &d [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return Between{&x, d[0]};                                 // expected-note {{param returned here}}
}

// Leading unnamed bit-field: the drift starts at the very first initializer.
struct [[gsl::Pointer(int)]] Leading {
  int : 3;
  const int *p;
  const int &r;
};

Leading leading(const int &x [[clang::lifetimebound]],
                const vector<int> &d [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return Leading{&x, d[0]};                                 // expected-note {{param returned here}}
}

// No bit-field: reported all along -- the three must agree.
struct [[gsl::Pointer(int)]] None {
  const int *p;
  const int &r;
};

None none(const int &x [[clang::lifetimebound]],
          const vector<int> &d [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return None{&x, d[0]};                              // expected-note {{param returned here}}
}

// A NAMED bit-field does consume an initializer, so skipping it would drift the other
// way. Also reported all along.
struct [[gsl::Pointer(int)]] Named {
  const int *p;
  int b : 3;
  const int &r;
};

Named named(const int &x [[clang::lifetimebound]],
            const vector<int> &d [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return Named{&x, 1, d[0]};                            // expected-note {{param returned here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: nothing that outlives the call is borrowed.
//===----------------------------------------------------------------------===//

static int g_anchor = 0;

Between from_global() {
  return Between{&g_anchor, g_anchor}; // no-warning
}

// A lifetimebound parameter bound to the reference member behind the bit-field is
// the truthful annotation, not an escape.
Between from_lifetimebound(const int &x [[clang::lifetimebound]]) {
  return Between{&x, x}; // no-warning
}
