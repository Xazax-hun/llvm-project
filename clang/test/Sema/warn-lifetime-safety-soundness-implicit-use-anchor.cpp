// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

volatile char sink;

// A borrow can be read by an *implicit* use: a non-trivial destructor running at
// scope exit, which has no source expression. issuePendingWarnings anchors a report
// on the loan's issuing expression, or on its placeholder parameter. The `$this`
// placeholder loan -- what a [[clang::lifetimebound]] accessor of `this` yields --
// has neither, and the implicit-use branch had no final `else`, so the invalidation
// was detected and then emitted nothing. The explicit-use branch already fell back
// to the use expression; an implicit use has none, so it anchors at the method the
// placeholder stands for.

struct [[gsl::Pointer]] Printer {
  string_view v;
  ~Printer(); // out-of-line: the analysis cannot see that it reads `v`
};

struct [[gsl::Owner]] Doc {
  string s;
  string_view text() const [[clang::lifetimebound]] { return s; }

  // The dangling read is the destructor's, so the use is implicit.
  // expected-warning@+1 {{borrow held by this object is later invalidated}}
  void implicit_use() {
    Printer p{text()};
    s.push_back('y'); // expected-note {{invalidated here}}
  }                   // expected-note {{later used here}}

  // Control: an explicit read of the same borrow was already reported, anchored at
  // the use rather than at the method.
  void explicit_use() {
    Printer p{text()};
    s.push_back('y');   // expected-note {{invalidated here}}
    sink = *p.v.data(); // expected-warning {{object whose reference is captured is later invalidated}} expected-note {{later used here}}
  }
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A view whose destructor is trivial reads nothing at scope exit, so there is no
// implicit use to report.
struct [[gsl::Pointer]] TrivialView {
  string_view v; // trivially destructible
};

struct [[gsl::Owner]] NoImplicitUse {
  string s;
  string_view text() const [[clang::lifetimebound]] { return s; }
  void ok() {
    TrivialView p{text()};
    s.push_back('y'); // no-warning: nothing reads `p` afterwards
  }
};

// Nothing is invalidated, so the destructor's read is fine.
struct [[gsl::Owner]] NoMutation {
  string s;
  string_view text() const [[clang::lifetimebound]] { return s; }
  void ok() {
    Printer p{text()}; // no-warning
  }
};
