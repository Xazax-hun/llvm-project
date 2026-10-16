// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-assumed-invalidation -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-unknown-ownership -verify=uo %s

#include "Inputs/lifetime-analysis.h"

// The analysis keeps type-only answers for the whole translation unit. One
// computed while a type is still incomplete must not outlive its completion:
// `early` asks whether Holder can reach a mutable owner while Fwd is only
// declared -- no -- and `late`, after Fwd gains a vector, must get the new
// answer, or the reallocation `mutate` may perform goes unreported.

volatile int sink;
struct Fwd;
struct [[gsl::Pointer]] Holder { Fwd *p; };
void mutate(Holder &h [[clang::noescape]]);

void early() {
  Holder h{nullptr};
  mutate(h);
}

struct Fwd { std::vector<int> v; };

void late() {
  Fwd f;
  Holder h{&f};
  int *q = &f.v[0]; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  mutate(h);        // expected-note {{assumed to be invalidated by this operation}}
  sink = *q;
}

// The same holds for the unknown-ownership memo: an incomplete type is
// conservatively of unknown ownership, but once completed with no borrow-holding
// member it is not, and a later function must not inherit the early answer.
struct Fwd2;
Fwd2 &makeRef2();
void early2() {
  Fwd2 &r = makeRef2(); // uo-warning {{type 'Fwd2' can hold a borrow but is annotated neither}}
  (void)r;
}
struct Fwd2 { int x; };
void late2() {
  Fwd2 &r = makeRef2();
  (void)r;
}
