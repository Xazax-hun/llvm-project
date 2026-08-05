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

// Same method: store into a member, invalidate, then read locally. Here the
// dangling read is in this function, so the dereference itself is a use and
// anchors the report -- more precise than the member declaration.
struct [[gsl::Owner]] SameMethod {
  SameMethod();
  ~SameMethod();
  void bad() {
    p = pv->data();
    pv->push_back(99); // expected-note {{invalidated here}}
    sink = *p;         // expected-warning {{object whose reference is captured is later invalidated}} expected-note {{later used here}}
  }

private:
  vector<int> *pv;
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

// A DESTRUCTOR's own cleanup must not report. By the time a destructor returns
// the object is gone, so nothing can read its members afterwards and "escapes to
// a field" is vacuous -- no field escape fact is emitted there at all. Otherwise
// every correct RAII owner would flag its own destructor.
struct [[gsl::Owner]] CorrectOwner {
  CorrectOwner() : pv(new vector<int>()) {}
  ~CorrectOwner() { delete pv; } // no-warning
  int size() const { return 0; }

private:
  vector<int> *pv;
};

// A member never used to take a borrow that is later invalidated stays clean.
struct [[gsl::Owner]] Untouched {
  Untouched();
  ~Untouched();
  void grow() { v.push_back(1); } // no-warning

private:
  vector<int> v; // an owner field, not a borrow-holding member
};
