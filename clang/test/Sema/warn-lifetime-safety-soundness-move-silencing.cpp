// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-move-silencing -verify %s

#include "Inputs/lifetime-analysis.h"

using std::string;
using std::string_view;
using std::unique_ptr;

// Moving an owner transfers ownership, which the analysis does not model, so it
// silences lifetime checks for the moved-from object.
void moving_owner_warns() {
  string s;
  string t = std::move(s); // expected-warning {{moving an owner is not modeled by lifetime safety analysis, so lifetime checks for the moved-from object are silenced}}
  (void)t;
}

void sink_view(string_view &&v);

// Moving a pointer-like view is just a copy: no warning.
void moving_view_is_silent() {
  string s;
  string_view v = s;
  sink_view(std::move(v)); // no-warning
}

// Moving a raw pointer is just a copy: no warning.
void moving_pointer_is_silent() {
  int x;
  int *p = &x;
  int *q = std::move(p); // no-warning
  (void)q;
}

// std::unique_ptr::release transfers ownership out of the owner.
void release_warns(unique_ptr<int> up) {
  int *raw = up.release(); // expected-warning {{moving an owner is not modeled by lifetime safety analysis}}
  (void)raw;
}
