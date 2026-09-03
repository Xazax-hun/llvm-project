// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

// A [[clang::noescape]] parameter's borrow stored into storage reached through
// ANOTHER parameter. The `this` spelling of the same body was always reported:
// member origins of the implicit object are seeded at entry, so the store lands
// on the FIELD's origin and function exit emits a field escape the noescape
// verifier consumes. A store through a parameter lands on a transient
// member-access origin instead -- owned by no declaration -- so no escape fact
// was emitted and nothing checked it, though the two bodies mean the same
// thing.
//
// A private member kept the type clear of the owner-public-borrow rule, so a
// [[gsl::Owner]] could adopt a caller's borrow with no diagnostic anywhere.

//===----------------------------------------------------------------------===//
// The two spellings of one body.
//===----------------------------------------------------------------------===//

class [[gsl::Owner]] BoxThis {
  std::string_view sv; // expected-note {{escapes to this field}}

public:
  void adopt(std::string_view s [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    sv = s;
  }
};

class [[gsl::Owner]] BoxParam {
  std::string_view sv;

public:
  static void adopt(BoxParam &b [[clang::noescape]],
                    std::string_view s [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    b.sv = s; // expected-note {{escapes into an object the caller owns here}}
  }
};

// A plain (non-owner) destination is the same escape.
struct Holder {
  std::string_view sv; // expected-note {{this field dangles}}
};

void into_plain_param(Holder &h [[clang::noescape]],
                      std::string_view s [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  h.sv = s; // expected-note {{escapes into an object the caller owns here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: no noescape parameter's borrow comes to rest in a caller's
// object.
//===----------------------------------------------------------------------===//

volatile char sink;

// Stored into a LOCAL: the local's expiry checks it; nothing escapes.
void into_local(std::string_view s [[clang::noescape]]) {
  Holder h; // expected-warning {{can hold a borrow but is annotated neither}}
  h.sv = s;
  sink = h.sv.data()[0];
}

// Read, not stored.
void just_read(std::string_view s [[clang::noescape]]) { sink = s.data()[0]; }

// Storing a borrow of a LOCAL into a parameter is a DIFFERENT bug from the
// noescape promise -- the local dangles, `s` itself does not escape -- and it is
// reported as such: a local's borrow in caller-owned storage dangles once the
// function returns.
void local_into_param(Holder &h [[clang::noescape]],
                      std::string_view s [[clang::noescape]]) {
  std::string tmp;
  h.sv = tmp; // expected-warning {{stack memory associated with local variable 'tmp' escapes to the field 'sv' which will dangle}}
}

// A store WITHIN one parameter (source and destination are the same object) is
// not an escape into a second caller object.
struct SelfHolder {
  std::string_view sv;
  std::string_view other;
};

void self_store(SelfHolder &h [[clang::noescape]]) {
  // expected-warning@+1 {{lifetime safety cannot track parameter 'h' here}}
  h.sv = h.other;
}
