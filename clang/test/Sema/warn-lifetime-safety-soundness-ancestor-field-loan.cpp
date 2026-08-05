// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

volatile char sink;

// A field mutation matches borrows by exact field identity, and falls back to a
// conservative "an imprecise borrow into the object is invalidated too" arm. That
// arm was suppressed whenever the borrow held *any* field loan, on the assumption
// that a borrow naming a field is matched exactly. That holds for a borrow naming
// the mutated field or a sibling of it, but not for one naming a field that
// CONTAINS the mutated one: `v = w.d.text()` carries field `d`'s loan while
// `w.d.s.assign(...)` names `s`, so nothing matched and the arm was skipped.
// Suppression now requires the field loan to be precise with respect to this
// mutation -- the mutated field itself, or disjoint from it.

struct [[gsl::Owner]] Doc {
  string s;
  string_view text() const [[clang::lifetimebound]] { return s; }
};
struct [[gsl::Owner]] Wrapper {
  Doc d;
};

// The borrow carries field `d`'s loan; the mutation names the inner field `s`.
void nested() {
  Wrapper w;
  string_view v = w.d.text(); // expected-warning {{object whose reference is captured is later invalidated}}
  w.d.s.push_back('y');       // expected-note {{invalidated here}}
  sink = *v.data();           // expected-note {{later used here}}
}

// Same, inside a method of the wrapper.
struct [[gsl::Owner]] Wrapper2 {
  Doc d;
  void bad() {
    string_view v = d.text();
    d.s.push_back('y'); // expected-note {{invalidated here}}
    sink = *v.data();   // expected-warning {{object whose reference is captured is later invalidated}} expected-note {{later used here}}
  }
};

// Control: the flat case already worked -- the borrow carries the loan of the very
// object being mutated.
void flat() {
  Doc d;
  string_view v = d.text(); // expected-warning {{object whose reference is captured is later invalidated}}
  d.s.push_back('y');       // expected-note {{invalidated here}}
  sink = *v.data();         // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Negatives: the precision the suppression exists to protect. A borrow naming a
// field disjoint from the mutated one must not be reported.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner]] Two {
  string a;
  string b;
};

void siblings() {
  Two t;
  string_view v = t.a;
  t.b.push_back('z'); // no-warning: `b` is disjoint from `a`
  sink = *v.data();
}

struct [[gsl::Owner]] Nest {
  Two x;
  Two y;
};

void nested_siblings() {
  Nest w;
  string_view v = w.x.a;
  w.y.b.push_back('z'); // no-warning: `y.b` is disjoint from `x.a`
  sink = *v.data();
}

// A borrow naming exactly the mutated field is matched by the exact path, not by
// the conservative arm.
void same_field() {
  Two t;
  string_view v = t.a; // expected-warning {{object whose reference is captured is later invalidated}}
  t.a.push_back('z');  // expected-note {{invalidated here}}
  sink = *v.data();    // expected-note {{later used here}}
}
