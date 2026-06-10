// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-view-on-mutable-global -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"

// A view (gsl::Pointer) created from a mutable global/static owner can be
// invalidated by mutating that owner elsewhere (another function or TU), which
// the intra-procedural analysis cannot see. Flag it. A const owner is safe.

std::string g_str;
const std::string g_const;

std::string_view from_mutable_global() {
  return g_str; // expected-warning {{view is created from a mutable global or static object}}
}

std::string_view from_const_global() {
  return g_const; // no-warning
}

std::string_view from_static_local() {
  static std::string s;
  return s; // expected-warning {{view is created from a mutable global or static object}}
}
