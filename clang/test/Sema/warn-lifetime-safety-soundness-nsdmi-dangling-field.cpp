// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-dangling-field -Wno-dangling-gsl -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

string make();

// A default member initializer (NSDMI) that binds a view member to a temporary
// dangles immediately: C++ does not lifetime-extend the temporary past the end
// of construction. The dangling field must be flagged regardless of *which*
// constructor applies the NSDMI -- including an implicit or =default
// constructor, which never reaches the normal per-function analysis path.
// (The legacy -Wdangling-gsl heuristic is silenced here to isolate the model.)

struct [[gsl::Pointer]] ImplicitCtor {
  string_view v = make(); // expected-warning {{stack memory associated with local temporary object escapes to the field 'v' which will dangle}} expected-note {{this field dangles}}
  // implicit default constructor applies the dangling NSDMI
};
void use_implicit() { ImplicitCtor c; (void)c; } // expected-note {{in implicit default constructor for 'ImplicitCtor' first required here}}

struct [[gsl::Pointer]] DefaultedCtor {
  string_view v = make(); // expected-warning {{stack memory associated with local temporary object escapes to the field 'v' which will dangle}} expected-note {{this field dangles}}
  DefaultedCtor() = default;
};
void use_defaulted() { DefaultedCtor c; (void)c; } // expected-note {{in defaulted default constructor for 'DefaultedCtor' first required here}}

// Control: a user-written constructor already reached the analysis; it must
// keep being flagged exactly once.
struct [[gsl::Pointer]] UserCtor {
  string_view v = make(); // expected-warning {{stack memory associated with local temporary object escapes to the field 'v' which will dangle}} expected-note {{this field dangles}}
  UserCtor() {}
};

// Control: binding to a non-temporary (a string literal has static storage) is
// not dangling and must stay silent.
struct [[gsl::Pointer]] SafeLiteral {
  string_view v = "literal"; // no-warning
};
void use_safe() { SafeLiteral c; (void)c; }
