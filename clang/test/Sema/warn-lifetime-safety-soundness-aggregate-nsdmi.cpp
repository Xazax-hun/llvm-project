// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-dangling-gsl -Wno-dangling -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A default member initializer is code, and through AGGREGATE initialization there is no
// constructor to carry it: the CXXDefaultInitExpr sits inline in the enclosing function,
// and its subexpression is in the CFG only when AddCXXDefaultInitExprInAggregates is
// asked for. Nothing inside it was ever handed to the analysis -- not refused, simply
// invisible -- so `Agg a{};` was silent while every constructor form was flagged.
//
// The initializer is now walked, so a hazard written inside one is reported wherever the
// aggregate is created. That also removed the need to replay refusals for this position:
// they arrive through the ordinary Visit path like anything else.

volatile char sink;
string make();

//===----------------------------------------------------------------------===//
// A borrow taken and lost inside the initializer.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] BindsTemporary {
  // expected-warning@+2 {{does not live long enough}}
  // expected-note@+1 {{destroyed here}}
  string_view v = make();
};
void aggregate_form() {
  BindsTemporary a{};
  sink = a.v.size() ? 1 : 0; // expected-note {{later used here}}
}

// Through the enclosing aggregate's own braces, and through an array of them: neither
// introduces a constructor either.
struct Holds {
  BindsTemporary inner;
  int k;
};
void nested_forms() {
  // An unannotated aggregate holding a view is separately reported as untrackable, and
  // the array form loses its borrow to the sentinel; both are about the enclosing
  // aggregate, not about the initializer being walked.
  Holds h{}; // expected-warning {{type 'Holds' can hold a borrow but is annotated neither}}
  BindsTemporary arr[1] = {};
  // expected-warning@+1 {{cannot track local variable 'arr' here}}
  sink = h.k + (arr[0].v.size() ? 1 : 0);
}

//===----------------------------------------------------------------------===//
// A refusal inside the initializer, which used to need replaying by hand.
//===----------------------------------------------------------------------===//

struct Wide {
  long a, b;
};
struct PunsInInitializer {
  char *p = nullptr;
  // expected-warning@+1 {{'reinterpret_cast' is not modeled by lifetime safety analysis}}
  long v = ((Wide *)p)->a;
};
void refusal_reaches_aggregate() {
  PunsInInitializer a{}; // expected-warning {{type 'PunsInInitializer' can hold a borrow but is annotated neither}}
  sink = (char)a.v;
}

union U {
  int i;
  float f;
};
struct ReadsUnionInInitializer {
  U u{};
  // expected-warning@+1 {{union member access is not modeled by lifetime safety analysis}}
  int v = u.i;
};
void union_reaches_aggregate() {
  ReadsUnionInInitializer a{};
  sink = (char)a.v;
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A borrow of storage that outlives the program.
struct [[gsl::Pointer]] BindsLiteral {
  string_view v = "literal"; // no-warning
};
void literal_is_fine() {
  BindsLiteral a{};
  // No dangling report, which is the point. The borrow is still lost to the sentinel on
  // use -- a separate, pre-existing gap in how a view built from a literal is tracked --
  // so this documents that rather than asserting silence it does not have.
  // expected-warning@+1 {{cannot track local variable 'a' here}}
  sink = a.v.size() ? 1 : 0;
}

// An initializer that borrows nothing.
struct Plain {
  int n = 41 + 1; // no-warning
  string s;       // no-warning
};
void plain_is_fine() {
  Plain a{};
  sink = (char)a.n;
}
