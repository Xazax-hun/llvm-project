// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-immortal-violation -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// [[clang::lifetime_immortal]] promises the return value lives forever, and --
// unlike [[clang::lifetimebound]] -- is otherwise trusted unverified. The body
// must therefore actually return immortal storage; returning a borrow of the
// implicit object, a parameter, or a local/temporary is a lie.

struct Cache {
  string data;
  // Lies: returns a view into this->data, which dies with the Cache.
  [[clang::lifetime_immortal]] string_view view() const { return data; } // expected-warning {{returns a borrow of the implicit this parameter}}
};

// Lies: returns a borrow of a parameter.
[[clang::lifetime_immortal]] string_view from_param(string_view s) { // expected-warning {{returns a borrow of a parameter}}
  return s;
}

// Lies: returns a borrow of a local.
[[clang::lifetime_immortal]] string_view from_local() { // expected-warning {{returns a borrow of a local or temporary}}
  string local;
  string_view v = local;
  return v;
}

// Lies via laundering: the returned view comes from an unannotated
// borrow-returning call, so its loan is untracked (Unknown) -- the analysis
// cannot prove it is immortal, even though here it actually borrows a local.
// An immortal function must return only provably-immortal storage, so an
// unverifiable borrow is a violation too. (Mirrors `return v.substr(0);` on a
// std::string_view, whose result is an untracked borrow.)
string_view launder(string_view); // unannotated: result is an untracked borrow
[[clang::lifetime_immortal]] string_view from_laundered() { // expected-warning {{returns a borrow of an object the analysis cannot prove is immortal}}
  string local;
  string_view v = local;
  return launder(v);
}

//===----------------------------------------------------------------------===//
// Negatives: the return really is immortal.
//===----------------------------------------------------------------------===//

string g_str;                 // global std::string: buffer freed at teardown
static const char G[] = "hi"; // static char array: genuinely immortal storage

// A view of a global whose type has a non-trivial destructor is NOT immortal:
// the buffer is freed at static destruction, so a caller keeping the result can
// read freed memory at teardown (and the destruction order is unknowable).
[[clang::lifetime_immortal]] string_view from_global() { return g_str; } // expected-warning {{returns a borrow of an object the analysis cannot prove is immortal}}

[[clang::lifetime_immortal]] const char *from_static() { return G; } // no-warning

// Composes: delegating to another immortal function yields an immortal (not
// untracked) loan, so it is verified and stays silent.
[[clang::lifetime_immortal]] string_view immortal_src();
[[clang::lifetime_immortal]] string_view delegate() { return immortal_src(); } // no-warning

// A non-immortal function returning a borrow of its object is fine here (this
// check only verifies lifetime_immortal bodies).
struct Plain {
  string data;
  string_view view() const [[clang::lifetimebound]] { return data; } // no-warning (immortal check)
};
