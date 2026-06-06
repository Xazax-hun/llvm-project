// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"

// A range-based for loop desugars to 'auto && __range = <range>; ...'. When the
// range is a view (gsl::Pointer, e.g. std::span / std::string_view) the
// compiler-introduced '__range' reference is a reference-to-view, which would
// otherwise be rejected as two levels of indirection. That reference merely
// aliases the range, so it must not count toward the one-level limit:
// range-for over a view is allowed (the view parameter must still be annotated,
// like any indirection).

int sum_span(std::span<int> s [[clang::noescape]]) {
  int t = 0;
  for (int x : s) // no-warning (was: '__range1' uses more than one level...)
    t += x;
  return t;
}

int sum_string_view(std::string_view s [[clang::noescape]]) {
  int t = 0;
  for (char c : s) // no-warning
    t += c;
  return t;
}

// Soundness is preserved: a user-written reference-to-view is NOT the
// compiler-introduced range variable, so it is still rejected as multi-level.
void explicit_view_ref(std::span<int> s) { // expected-warning {{parameter that can hold a borrow is not annotated}}
  std::span<int> &r = s; // expected-warning {{'r' uses more than one level of indirection}}
  (void)r;
}

// A genuinely multi-level local is still rejected.
void multilevel(int **pp) { // expected-warning {{'pp' uses more than one level of indirection}}
  (void)pp;
}
