// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

string make();

// A [[clang::lifetimebound]] function returning a reference TO a view (a
// const string_view& -- two levels of indirection), declared without a body so
// it is not analyzed -- exactly the shape of std::max/min/clamp. The
// lifetimebound flow constrains only the top-level (reference) origin; the inner
// level (the view's own borrow) is seeded with an Unknown loan, so the dropped
// inner borrow is reported as lost rather than silently empty.
const string_view &pick(const string_view &a [[clang::lifetimebound]],
                        const string_view &b [[clang::lifetimebound]]);

// Straight-line: the copied-out view has an untracked (Unknown) inner borrow.
char direct() {
  string_view sv = pick(string_view(), string_view(make()));
  return sv.data()[0]; // expected-warning {{lifetime safety cannot track local variable 'sv'}}
}

// A control-flow merge must NOT mask it: although the if-branch supplies a valid
// loan, the Unknown loan from the else-branch survives the dataflow join.
char masked(int c) {
  string keep = make();
  string_view sv;
  if (c)
    sv = string_view(keep); // valid loan on this path
  else
    sv = pick(string_view(), string_view(make())); // untracked inner borrow
  return sv.data()[0]; // expected-warning {{lifetime safety cannot track local variable 'sv'}}
}
