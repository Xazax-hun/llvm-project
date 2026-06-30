// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-owner-of-indirection -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string_view;

namespace std {
// Minimal stand-in: recognized by name (std::variant). Its value lives in a
// union/type-erased buffer the analysis does not expand, so a view alternative
// is untracked -- the bug this check closes.
template <class... Ts> class variant {
  char buf[32];
};
} // namespace std

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
