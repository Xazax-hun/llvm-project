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

//===----------------------------------------------------------------------===//
// Heap storage is not immortal.
//===----------------------------------------------------------------------===//

// `new` gives storage that lives until someone frees it, and the intra-procedural
// analysis cannot see whether anyone does: the allocation is handed to the caller,
// and a `delete` anywhere -- typically in the destructor of the object that cached
// it, or of an owner the result is later given to -- ends its lifetime while
// callers still trust the immortal promise. Storage *duration* alone does not make
// a borrow immortal, exactly as for a global with a non-trivial destructor above.
struct Arena {
  int *p = nullptr;
  ~Arena() { delete p; } // frees what alloc() handed out
  // Lies: the result lives only as long as *this.
  [[clang::lifetime_immortal]] int *alloc() { // expected-warning {{returns a borrow of an object the analysis cannot prove is immortal}}
    int *n = new int(5);
    p = n;
    return n;
  }
};

// Returning the allocation directly is equally unprovable.
[[clang::lifetime_immortal]] int *fresh() { // expected-warning {{returns a borrow of an object the analysis cannot prove is immortal}}
  return new int(1);
}

// A static-cached allocation is the same: nothing in the body distinguishes a
// deliberately leaked allocation from one that is freed later. The immortal
// alternative is a static of trivially-destructible type (see from_static above),
// which needs no allocation at all.
[[clang::lifetime_immortal]] int *cached() { // expected-warning {{returns a borrow of an object the analysis cannot prove is immortal}}
  static int *c = new int(2);
  return c;
}

// Reaching into the allocation does not launder it either.
struct Box { int field; };
[[clang::lifetime_immortal]] int *field_of_new() { // expected-warning {{returns a borrow of an object the analysis cannot prove is immortal}}
  return &(new Box{3})->field;
}

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
