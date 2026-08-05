// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

volatile char sink;

// invalidatedObjectRecord answers "which record does this loan's object have?",
// and the conservative "an imprecise borrow into the object is invalidated too"
// arm of the invalidation check is skipped when it returns null. It handled the
// `$this` placeholder and a plain ValueDecl, but AccessPath::getAsValueDecl() is
// gated on Kind::ValueDecl and returns null for a *parameter* placeholder -- whose
// root is a ParmVarDecl. So a borrow taken through a reference/pointer PARAMETER
// yielded no record and the arm never ran, even though the identical code with a
// `this` or a local receiver was reported.

struct [[gsl::Owner]] Doc {
  string s;
  // An imprecise borrow: bound to the object, but which subobject is unknown.
  string_view text() const [[clang::lifetimebound]] { return s; }
};

// The receiver is a parameter: this was silent.
// expected-warning@+1 {{parameter is later invalidated}}
void via_param(Doc &d [[clang::noescape]]) {
  string_view v = d.text();
  d.s.push_back('y'); // expected-note {{invalidated here}}
  sink = *v.data();   // expected-note {{later used here}}
}

// A pointer parameter is peeled the same way.
// expected-warning@+1 {{parameter is later invalidated}}
void via_ptr_param(Doc *d [[clang::noescape]]) {
  string_view v = d->text();
  d->s.push_back('y'); // expected-note {{invalidated here}}
  sink = *v.data();    // expected-note {{later used here}}
}

// Controls: these already worked, and must keep working.
struct [[gsl::Owner]] ViaThis {
  string s;
  string_view text() const [[clang::lifetimebound]] { return s; }
  void bad() {
    string_view v = text();
    s.push_back('y'); // expected-note {{invalidated here}}
    sink = *v.data(); // expected-warning {{object whose reference is captured is later invalidated}} expected-note {{later used here}}
  }
};

void via_local() {
  Doc d;
  string_view v = d.text(); // expected-warning {{object whose reference is captured is later invalidated}}
  d.s.push_back('y');       // expected-note {{invalidated here}}
  sink = *v.data();         // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// The record now yielded by a parameter placeholder also covers the callee side
// of the `this`-aliasing hazard: a non-const call on a parameter whose type can
// reach the borrowed field may invalidate it, because the callee cannot know the
// parameter does not alias `this`.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner]] Registry {
  vector<string> items;
  void wipe() { items.clear(); }
  void report(Registry *other [[clang::noescape]]) {
    string_view sv = items[0]; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    other->wipe();             // expected-note {{assumed to be invalidated by this operation}}
    sink = *sv.data();
  }
};

// Negative: a parameter of an unrelated type cannot reach the borrowed field, so
// the record test excludes it -- this is targeted, not a blanket rule.
struct Unrelated {
  int n;
  void bump() { ++n; }
};

struct [[gsl::Owner]] Targeted {
  vector<string> items;
  void ok(Unrelated *u [[clang::noescape]]) {
    string_view sv = items[0];
    u->bump(); // no-warning: 'Unrelated' cannot reach 'items'
    sink = *sv.data();
  }
};

// Negative: a const parameter cannot mutate, so nothing is assumed.
struct [[gsl::Owner]] ConstParam {
  vector<string> items;
  void look() const {}
  void ok(const ConstParam *other [[clang::noescape]]) {
    string_view sv = items[0];
    other->look(); // no-warning
    sink = *sv.data();
  }
};
