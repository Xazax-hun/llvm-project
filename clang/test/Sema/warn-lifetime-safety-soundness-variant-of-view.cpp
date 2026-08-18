// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-owner-of-indirection -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string_view;

// `std::variant` comes from the shared header, which is treated as a system header so
// its stand-ins count as library code (a declaration in namespace `std` is trusted only
// when the library wrote it). Its value lives in a type-erased buffer the analysis does
// not expand, so a view alternative is untracked -- the bug this check closes.

struct [[gsl::Pointer]] View { const char *p; };

// A std::variant whose alternatives include a borrow (view/pointer/reference) is
// a container of indirection the analysis cannot track per alternative.
void view_alt() {
  std::variant<int, string_view> v; // expected-warning {{is a container whose element type holds a borrow}}
  (void)v;
}
void ptr_alt() {
  std::variant<int, const char *> v; // expected-warning {{is a container whose element type holds a borrow}}
  (void)v;
}
void gslptr_alt() {
  std::variant<int, View> v; // expected-warning {{is a container whose element type holds a borrow}}
  (void)v;
}

// Controls: alternatives that are not borrows must stay clean.
void scalar_alts() {
  std::variant<int, double> v; // no-warning
  (void)v;
}
void owner_alt() {
  std::variant<int, std::string> v; // no-warning: an owner alternative is not a borrow
  (void)v;
}
