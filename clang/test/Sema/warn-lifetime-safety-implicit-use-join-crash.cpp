// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-invalidation -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// Regression: a non-trivial destructor at scope exit is modeled as an implicit
// use (a UseFact with no source expression). The deterministic-join helper in
// the live-origins analysis must read that fact's explicit location rather than
// dereferencing its null use-expression -- otherwise analyzing a function with
// both a tracked borrow and such an implicit-destructor use (e.g. a
// std::function local, whose closure type has a non-trivial destructor) crashed.
// This test must simply compile without crashing; the invalidation is also
// diagnosed.

void function_with_view_and_callable() {
  string text = "long enough heap-allocated backing string value here!!!!";
  string_view tok = text; // a tracked borrow (creates a live origin)
  std::function<void()> c = [] {}; // non-trivial dtor at scope exit
  (void)c;
  (void)tok.size(); // no-warning (must not crash)
}

void function_with_view_mutation() {
  string text = "long enough heap-allocated backing string value here!!!!";
  string_view tok = text; // expected-warning {{object whose reference is captured is later invalidated}}
  std::function<void()> c = [] {};
  (void)c;
  text.push_back('x'); // expected-note {{invalidated here}}
  (void)tok.size();    // expected-note {{later used here}}
}
