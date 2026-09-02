// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A temporary bound to a reference member by a DEFAULT MEMBER INITIALIZER is
// lifetime-extended to the enclosing object, so it dies with the variable that
// object is -- the AST records it as "extended by Var 'a'", not by the field.
//
// The expiry is collected by walking the variable's initializer for extended
// temporaries, and that walk descended through `Stmt::children()`. But a default
// initializer holds its expression on the FIELD, so CXXDefaultInitExpr::children()
// is an empty range by construction and the walk never entered it. The temporary
// was therefore never collected and never expired: its loan was tracked correctly
// all the way into the borrower and then simply never died, so a use after the
// object's scope was invisible. Zero diagnostics, not a refusal.
//
// The same walk now steps through a default initializer's expression (and a
// default argument's).

volatile char sink;

string mk() { return string("a long heap string value exceeding the sso buffer"); }

// Each case gets its own struct: the diagnostic anchors at the borrow's CREATION
// site, which is the default initializer inside the class, not the declaration of
// the variable that outlives it.

struct [[gsl::Pointer(char)]] AggEscape {
  const string &r = mk(); // expected-warning {{local temporary object does not live long enough}}
};

// The borrow outlives the object the temporary was extended to.
void escapes_the_object() {
  string_view out;
  {
    AggEscape a{};
    out = a.r;
  }                     // expected-note {{destroyed here}}
  sink = out.data()[0]; // expected-note {{later used here}}
}

struct [[gsl::Pointer(char)]] AggReturn {
  const string &r = mk(); // expected-warning {{stack memory associated with local temporary object is returned}}
};

// Returning a view of it is the same borrow outliving the same temporary.
string_view returns_view_of_temp() {
  AggReturn a{};
  return a.r; // expected-note {{returned here}}
}

// Nested one level down. Clang re-targets the extension to the VARIABLE when the
// temporary is one level down, but leaves it on the FIELD once another aggregate
// sits in between -- either way the storage dies with the variable.
struct [[gsl::Pointer(char)]] AggNested {
  const string &r = mk(); // expected-warning {{local temporary object does not live long enough}}
};
struct [[gsl::Pointer(char)]] Outer {
  AggNested inner{};
};

void escapes_through_nesting() {
  string_view out;
  {
    Outer o{};
    out = o.inner.r;
  }                     // expected-note {{destroyed here}}
  sink = out.data()[0]; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(char)]] AggQuiet {
  const string &r = mk(); // no-warning
};

// The temporary is extended to the OBJECT, so it lives as long as the object
// does -- reading through the member while the object is alive is fine. This is
// why the expiry belongs at the variable's scope end and not at the end of the
// full-expression that built it; ASan agrees the read here is valid.
void read_while_object_alive() {
  AggQuiet a{};
  sink = a.r.data()[0]; // no-warning
}

// The borrow does not outlive the object.
void borrow_dies_first() {
  AggQuiet a{};
  {
    string_view out = a.r;
    sink = out.data()[0]; // no-warning
  }
}

// A member bound to a real object extends nothing.
struct [[gsl::Pointer(char)]] Bound {
  const string &r;
  explicit Bound(const string &s [[clang::lifetimebound]]) : r(s) {}
};

void bound_to_a_real_object() {
  string owner = "a long heap string value exceeding the sso buffer";
  Bound b{owner};
  sink = b.r.data()[0]; // no-warning
}
