// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::vector;

// FinalWarningsMap is keyed by loan, so one entry per loan survives, and the
// first entry with a dominating causing fact used to win outright. That silently
// dropped diagnostics for a loan with no anchor of its own.
//
// Every pointer/view member of `this` is seeded at entry with a non-expiring
// "uninitialized" loan created with no issuing expression and
// AccessPath::Kind::Uninitialized, so it has neither of the two anchors the
// escape-caused report branches handle (an issuing expression or a placeholder
// *parameter*). A borrow taken THROUGH such a member carries that loan. The
// member's own origin is live at function exit, so it produced an escape-caused
// entry first -- which claimed the slot and then emitted nothing -- masking the
// use-caused entry for the same loan, which does have a working use anchor.
//
// A reportable entry now takes over even from a dominating unreportable one.

struct [[gsl::Owner]] Box {
  Box();
  ~Box();
  int bad() {
    const int *q = pv->data();
    pv->push_back(99); // expected-note {{invalidated here}}
    return *q;         // expected-warning {{object whose reference is captured is later invalidated}} expected-note {{later used here}}
  }

private:
  vector<int> *pv; // raw pointer member: seeded, so its loan has no anchor
};

// Same shape with a string owner reached through the pointer member.
struct [[gsl::Owner]] Doc {
  Doc();
  ~Doc();
  char bad() {
    const char *d = s->data();
    s->push_back('b'); // expected-note {{invalidated here}}
    return *d;         // expected-warning {{object whose reference is captured is later invalidated}} expected-note {{later used here}}
  }

private:
  string *s;
};

// Reached through a pointer member inherited from a base.
struct BoxBase {
protected:
  vector<int> *pv;
};
struct [[gsl::Owner]] DerivedBox : BoxBase {
  DerivedBox();
  ~DerivedBox();
  int bad() {
    const int *q = pv->data();
    pv->push_back(99); // expected-note {{invalidated here}}
    return *q;         // expected-warning {{object whose reference is captured is later invalidated}} expected-note {{later used here}}
  }
};

//===----------------------------------------------------------------------===//
// Controls: already reported before, and must stay reported. They isolate the
// seeded-member loan as the thing that had no anchor -- both of these loans do
// have one, so they anchor at the borrow rather than at the use.
//===----------------------------------------------------------------------===//

// A local pointer to the container: the loan roots at the `new` allocation, which
// gives it an issuing expression.
int local_ptr() {
  vector<int> *pv = new vector<int>(); // expected-warning {{object whose reference is captured is later invalidated}}
  const int *q = pv->data();
  pv->push_back(99); // expected-note {{invalidated here}}
  int r = *q;
  delete pv; // expected-note {{later used here}}
  return r;
}

// A parameter: the loan roots at a placeholder parameter, the other anchor the
// escape branches already handled.
int param_ptr(vector<int> *pv [[clang::noescape]]) { // expected-warning {{parameter is later invalidated}}
  const int *q = pv->data();
  pv->push_back(99); // expected-note {{invalidated here}}
  return *q;         // expected-note {{later used here}}
}
