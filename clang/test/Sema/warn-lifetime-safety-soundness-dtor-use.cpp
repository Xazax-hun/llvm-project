// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A [[gsl::Pointer]] whose NON-TRIVIAL destructor may read a borrow it holds.
// The analysis is intra-procedural and does not see the (out-of-line) destructor
// body, so scope-exit destruction of such an object is modeled as a *use* of it:
// a borrow it captured stays live up to its destruction, so a borrowed-from
// object destroyed earlier (reverse-declaration order) is flagged.

struct [[gsl::Pointer]] Ref {
  string_view sv;
  Ref() = default;
  Ref &operator=(string_view s [[clang::lifetime_capture_by(this)]]);
  ~Ref(); // non-trivial, out-of-line: may read sv
};

void captured_then_destroyed_in_order() {
  Ref r;                  // declared first -> destroyed LAST
  string backing;         // declared second -> destroyed FIRST
  r = backing;            // expected-warning {{does not live long enough}}
}                         // expected-note {{destroyed here}} expected-note {{later used here}}

// Safe: backing outlives r (r destroyed first, while backing is still alive).
void captured_safe_order() {
  string backing;
  Ref r;
  r = backing; // no-warning
}

// A view with a TRIVIAL destructor cannot read the borrow at destruction, so a
// dangling borrow held only "across" its destruction (never explicitly used) is
// not a destructor-use concern (an explicit use would be caught separately).
struct [[gsl::Pointer]] TrivialRef {
  string_view sv;
  TrivialRef &operator=(string_view s [[clang::lifetime_capture_by(this)]]);
  // trivial (implicit) destructor
};

void trivial_dtor_no_use() {
  TrivialRef r;
  string backing;
  r = backing; // no-warning (trivial destructor does not read sv)
}
