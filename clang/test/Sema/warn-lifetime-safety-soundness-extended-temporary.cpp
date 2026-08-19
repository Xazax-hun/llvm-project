// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-dangling -Wno-dangling-gsl -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A loan was issued for a temporary only when its storage duration was SD_FullExpression.
// Binding one to a reference LIFETIME-EXTENDS it -- SD_Automatic -- and no loan was created
// at all, leaving the reference's origin empty rather than carrying anything.
//
// Two consequences, and the second is what made this quiet. Nothing could expire, since
// there was no loan to expire. And the `lost-loan` sentinel needs the origin to be entirely
// empty, so any co-resident real loan masked it: a conditional whose other arm carries a
// tracked borrow was enough to hide the bug outright.
//
// The temporary's storage is a borrow root like any other, so it gets a loan whatever its
// duration, and one extended by binding to a local reference expires with that reference's
// scope.

string make();
volatile char sink;

//===----------------------------------------------------------------------===//
// The borrow outliving the extending reference is reported, precisely.
//===----------------------------------------------------------------------===//

int extendedTemporaryEscapesScope() {
  string_view v;
  {
    // expected-warning@+1 {{does not live long enough}}
    const string &r = make();
    v = r;
  } // expected-note {{destroyed here}}
  return v.size(); // expected-note {{later used here}}
}

// The masked form: the live arm's loan is what used to hide the temporary arm's absence.
// Reported by reasoning about the lifetime now, not by the blanket sentinel.
int extendedTemporaryMaskedAtJoin(bool cond) {
  string live = "live";
  string_view v;
  {
    // expected-warning@+1 {{does not live long enough}}
    const string &r = cond ? live : static_cast<const string &>(make());
    v = r;
  } // expected-note {{destroyed here}}
  return v.size(); // expected-note {{later used here}}
}

// An rvalue reference extends the same way.
int extendedTemporaryRValueRef() {
  string_view v;
  {
    // expected-warning@+1 {{does not live long enough}}
    string &&r = make();
    v = r;
  } // expected-note {{destroyed here}}
  return v.size(); // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Negatives: extension doing its job must stay clean.
//===----------------------------------------------------------------------===//

// Used entirely within the extending reference's scope. This is the whole point of
// lifetime extension, and it used to draw a spurious lost-loan because the origin was
// empty rather than holding a live loan.
char extendedTemporaryUsedInScope() {
  const string &r = make(); // no-warning
  return r.data()[0];
}

char extendedTemporaryNestedScope() {
  char c = 0;
  {
    const string &r = make(); // no-warning
    c = r.data()[0];
  }
  return c;
}

// Binding to a named local rather than a temporary was always tracked.
char referenceToNamedLocal() {
  string s = "named";
  const string &r = s; // no-warning
  return r.data()[0];
}

// Static duration: the storage outlives every use in this function, so the loan never
// expires here. (Whether such an object is safe to DESTROY at shutdown is the
// destruction-order rule's question, not this one.)
char extendedTemporaryStaticDuration() {
  static const string &r = make(); // no-warning
  return r.data()[0];
}
