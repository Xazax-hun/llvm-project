// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

volatile char sink;

// A mutating call whose receiver is a data member scopes the invalidation to that
// field, since the receiver's origin also carries the enclosing object's loan. That
// is right for a member holding its own storage, but wrong for a *reference*
// member: a reference is an alias, so the receiver's loan names the referent, not
// the field. Field-identity then matched nothing -- and the early return also
// skipped the generic access-path comparison that does match -- so mutating through
// a reference member invalidated nothing. The pointer spelling of the same design
// always worked, because its receiver is a dereference rather than a MemberExpr.

struct [[gsl::Pointer]] RefCtx {
  vector<string> &items;
};

void via_ref_member() {
  vector<string> v;
  RefCtx c{v}; // expected-warning {{object whose reference is captured is later invalidated}}
  string_view sv = c.items[0];
  c.items.clear(); // expected-note {{invalidated here}}
  sink = *sv.data(); // expected-note {{later used here}}
}

// The borrow may equally be taken through the container's own name.
void borrow_via_own_name() {
  vector<string> v;
  RefCtx c{v};
  string_view sv = v[0]; // expected-warning {{object whose reference is captured is later invalidated}}
  c.items.clear(); // expected-note {{invalidated here}}
  sink = *sv.data(); // expected-note {{later used here}}
}

// Controls: both spellings that already worked.
struct [[gsl::Pointer]] PtrCtx {
  vector<string> *items;
};

void via_ptr_member() {
  vector<string> v;
  PtrCtx c{&v}; // expected-warning {{object whose reference is captured is later invalidated}}
  string_view sv = (*c.items)[0];
  c.items->clear(); // expected-note {{invalidated here}}
  sink = *sv.data(); // expected-note {{later used here}}
}

void via_own_name() {
  vector<string> v;
  RefCtx c{v}; // expected-warning {{object whose reference is captured is later invalidated}}
  string_view sv = c.items[0];
  v.clear(); // expected-note {{invalidated here}}
  sink = *sv.data(); // expected-note {{later used here}}
}

// A by-value owner member still gets field-scoped invalidation, so a borrow taken
// through a disjoint reference member is not reported.
struct [[gsl::Pointer]] Mixed {
  vector<string> &r;
  vector<string> owned;
};

void by_value_sibling_is_disjoint() {
  vector<string> v;
  Mixed m{v, {}};
  string_view sv = m.r[0];
  m.owned.clear(); // no-warning: `owned` holds its own storage, disjoint from `v`
  sink = *sv.data();
}

// KNOWN IMPRECISION: a [[gsl::Pointer]] type is a leaf in the origin tree, so its
// reference members share the aggregate's single origin and each carries the union
// of all referents' loans. With two reference members bound to *different* objects
// the analysis cannot tell them apart, so mutating one reports borrows from the
// other even though that is safe. Distinguishing them requires expanding a
// gsl::Pointer's fields into separate origins. The same imprecision is what lets
// the genuinely-aliased case (both members bound to the same object) be caught.
struct [[gsl::Pointer]] TwoRefs {
  vector<string> &a;
  vector<string> &b;
};

void two_refs_distinct_targets() {
  vector<string> v1, v2;
  TwoRefs t{v1, v2}; // expected-warning 2 {{object whose reference is captured is later invalidated}}
  string_view sv = t.a[0];
  t.b.clear(); // expected-note 2 {{invalidated here}}
  sink = *sv.data(); // expected-note 2 {{later used here}}
}
