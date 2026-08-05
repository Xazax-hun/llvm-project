// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

volatile int sink;

// A borrow taken through a pointer/view MEMBER of `this` carries the seed loan
// issued at entry, which has no issuing expression and is not rooted at a
// parameter. The escape-caused report branches handled only those two anchors, so
// when the borrow was stored into another member -- leaving the field escape at
// exit as the only causing fact -- the detected invalidation was silently dropped.
//
// The seed's access path names the member the borrow came through, which is a
// usable anchor. Reporting against it also makes the *cross-method* shapes
// reachable: the fields are already considered escaped at the end of the method,
// so a borrow cached in a member and invalidated before the method returns is
// reported even though nothing reads it in this function.

struct [[gsl::Owner]] Box {
  Box();
  ~Box();
  // Cache and invalidate in one method; the read happens in another. Nothing in
  // this function uses `p`, so only the field escape is available.
  void cache_then_invalidate() {
    p = pv->data();
    pv->push_back(99); // expected-note {{invalidated here}}
  }
  int read() const { return *p; }

private:
  vector<int> *pv; // expected-warning {{borrow held by this member which escapes to a field is later invalidated}} expected-note {{this field dangles}}
  const int *p;
};

// Same method: store into a member, invalidate, then read locally.
struct [[gsl::Owner]] SameMethod {
  SameMethod();
  ~SameMethod();
  void bad() {
    p = pv->data();
    pv->push_back(99); // expected-note {{invalidated here}}
    sink = *p;
  }

private:
  vector<int> *pv; // expected-warning {{borrow held by this member which escapes to a field is later invalidated}} expected-note {{this field dangles}}
  const int *p;
};

// A view member holding a borrow into a string reached through a pointer member.
struct [[gsl::Owner]] Doc {
  Doc();
  ~Doc();
  void refresh() {
    view = *s;
    s->push_back('b'); // expected-note {{invalidated here}}
  }
  char first() const { return *view.data(); }

private:
  string *s; // expected-warning {{borrow held by this member which escapes to a field is later invalidated}} expected-note {{this field dangles}}
  string_view view;
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A hand-rolled owner freeing its own raw pointer member in its destructor DOES
// report: the member is left dangling at exit, which is harmless there, but the
// field escape at the end of a destructor is real (member and base destructors run
// after the body and can read what it stored) and it is also the only thing that
// keeps a member-held borrow live across an in-body invalidation. Suppressing
// these facts in destructors was tried and lost several genuine bug classes, so
// this report is accepted; the idiomatic `unique_ptr` or by-value owner member
// below does not produce it.
struct [[gsl::Owner]] RawOwner {
  RawOwner();
  ~RawOwner() { delete pv; } // expected-note {{freed here}}

private:
  vector<int> *pv; // expected-warning {{borrow held by this member which escapes to a field is later invalidated}} expected-note {{this field dangles}}
};

// The idiomatic forms stay clean.
struct [[gsl::Owner]] CorrectOwner {
  CorrectOwner();
  ~CorrectOwner();
  int size() const { return 0; } // no-warning

private:
  vector<int> byValue; // an owner field, no raw owning pointer
};

// A member never used to take a borrow that is later invalidated stays clean.
struct [[gsl::Owner]] Untouched {
  Untouched();
  ~Untouched();
  void grow() { v.push_back(1); } // no-warning

private:
  vector<int> v; // an owner field, not a borrow-holding member
};

//===----------------------------------------------------------------------===//
// Destructors. Fields escape a destructor too: member and base destructors run
// after the body and can read what it stored, and the escape is also what keeps a
// member-held borrow live across an in-body invalidation. These all regressed once
// when destructor field escapes were suppressed.
//===----------------------------------------------------------------------===//

// Borrow into *pv held in a member, invalidated and read inside the destructor.
struct [[gsl::Owner]] DtorBorrow {
  DtorBorrow();
  ~DtorBorrow() {
    p = pv->data();
    pv->push_back(99); // expected-note {{invalidated here}}
    sink = *p;
  }

private:
  vector<int> *pv; // expected-warning {{borrow held by this member which escapes to a field is later invalidated}} expected-note {{this field dangles}}
  const int *p;
};

// Stored by a method, invalidated and read by the destructor.
struct [[gsl::Owner]] DtorCrossMethod {
  DtorCrossMethod();
  void cache() { p = pv->data(); }
  ~DtorCrossMethod() {
    pv->push_back(99); // expected-note {{invalidated here}}
    sink = *p;
  }

private:
  vector<int> *pv; // expected-warning {{borrow held by this member which escapes to a field is later invalidated}} expected-note {{this field dangles}}
  const int *p;
};

// A borrow of a LOCAL stored into a member by the destructor body, then read by
// that member's own destructor -- which runs after the body. This is the case that
// disproves "nothing can read the members after a destructor returns".
struct ReadsOnDestroy {
  string_view v;
  ~ReadsOnDestroy();
};
struct [[gsl::Owner]] DtorStoresLocal {
  ~DtorStoresLocal() {
    string tmp;
    in.v = tmp; // expected-warning {{escapes to the field 'in' which will dangle}}
  }

private:
  ReadsOnDestroy in; // expected-note {{this field dangles}}
};
