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

//===----------------------------------------------------------------------===//
// A member's destructor runs AFTER the enclosing destructor's body, so it can
// read what that body stored. Modeling the member's destruction as a use of the
// member keeps such a borrow live up to that point.
//===----------------------------------------------------------------------===//

struct ReadsOnDestroy2 {
  string_view v;
  ~ReadsOnDestroy2(); // out-of-line: the analysis cannot see that it reads `v`
};

struct [[gsl::Owner]] StoresLocalInDtor {
  ~StoresLocalInDtor() {
    string tmp;
    in.v = tmp; // expected-warning {{local variable 'tmp' does not live long enough}}
  }             // expected-note {{destroyed here}} expected-note {{later used here}}

private:
  ReadsOnDestroy2 in;
};

// The same store in an ordinary member function, for contrast: there no member
// destructor runs, so the borrow is reported through the field escape at exit
// instead -- a different diagnostic for the same underlying store.
struct [[gsl::Owner]] StoresLocalInMethod {
  void bad() {
    string tmp;
    in.v = tmp; // expected-warning {{stack memory associated with local variable 'tmp' escapes to the field 'in' which will dangle}}
  }

private:
  ReadsOnDestroy2 in; // expected-note {{this field dangles}}
};

// Negative: a member whose destructor is trivial cannot read anything, so its
// destruction is not a use.
struct TrivialDtor {
  string_view v; // trivially destructible
};

struct [[gsl::Owner]] TrivialMemberIsNotAUse {
  ~TrivialMemberIsNotAUse() {
    string tmp;
    in.v = tmp; // no-warning: nothing reads `in` after the body
  }

private:
  TrivialDtor in;
};

// Negative: an owner member's destruction frees its own storage (already modeled
// by expiry) rather than reading a borrow into something else.
struct [[gsl::Owner]] OwnerMemberIsNotAUse {
  ~OwnerMemberIsNotAUse() {} // no-warning

private:
  string owned;
};
