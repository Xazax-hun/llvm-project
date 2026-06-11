// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-view-on-mutable-global -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"

// A view (gsl::Pointer) created from a mutable global/static owner can be
// invalidated by mutating that owner elsewhere (another function or TU), which
// the intra-procedural analysis cannot see. Flag it. A const owner is safe.

std::string g_str;
const std::string g_const;

std::string_view from_mutable_global() {
  return g_str; // expected-warning {{borrows from a mutable global or static object}}
}

std::string_view from_const_global() {
  return g_const; // no-warning
}

std::string_view from_static_local() {
  static std::string s;
  return s; // expected-warning {{borrows from a mutable global or static object}}
}

// The same applies to a raw pointer/reference that borrows *into* a mutable
// global owner's contents (not only a GSL view). The borrow may be invalidated
// by a mutation of the global elsewhere.

std::vector<int> g_vec;

int *raw_into_global() {
  return &g_vec[0]; // expected-warning {{borrows from a mutable global or static object}}
}

void raw_into_global_local() {
  int *p = &g_vec[0];
  (void)p; // expected-warning {{borrows from a mutable global or static object}}
}

std::vector<int> *pointer_at_global() {
  return &g_vec; // no-warning (points at the object, whose storage is stable)
}

void borrow_into_local() {
  std::vector<int> l;
  int *p = &l[0];
  (void)p; // no-warning (local, not a global)
}

// A view into the CONTENTS of a mutable global owner -- e.g. a std::string
// element of a global std::vector reached via operator[] -- borrows from the
// global just as directly as a view of the whole owner. The borrow surfaces as
// a loan rooted at the global, so it is caught regardless of how it was reached.

std::vector<std::string> g_table;

std::string_view element_of_global_returned(int i) {
  return g_table[i]; // expected-warning {{borrows from a mutable global or static object}}
}

void element_of_global_local(int i) {
  std::string_view v = g_table[i]; // expected-warning {{borrows from a mutable global or static object}}
  (void)v; // expected-warning {{borrows from a mutable global or static object}}
}

// A [[clang::lifetime_immortal]] accessor does not exempt this: the attribute
// promises the function's *result* outlives callers, but a global owner's
// reallocatable buffer is not immortal. The loan-based check still sees the
// loan rooted at the global.
[[clang::lifetime_immortal]] std::string_view immortal_element(int i) {
  return g_table[i]; // expected-warning {{borrows from a mutable global or static object}}
}

// Precision: a view into a LOCAL container's element is not a global borrow.
void element_of_local() {
  std::vector<std::string> l;
  std::string_view v = l[0];
  (void)v; // no-warning (local, not a global)
}


