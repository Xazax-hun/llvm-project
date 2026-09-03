// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A C++23 static `operator()` / `operator[]` is still written with object syntax,
// so a CXXOperatorCallExpr carries the object expression as argument 0 even
// though the callee has no implicit object parameter and its own parameters start
// at 0. The object argument has to be dropped, or every parameter is off by one
// and the first one's [[clang::lifetimebound]] is attributed to the OBJECT --
// which typically outlives the call, so the dangle goes unreported.
//
// That drop was gated on `operator()` by name, so `static operator[]` (the other
// operator C++23 allows to be static) kept the off-by-one: `r[k]` claimed its
// result borrowed `r`, not `k`. The question is now whether the callee is a
// static MEMBER, so a future static operator needs no change here.
//
// `isStatic()` alone would not do. A file-static FREE operator is static too and
// its argument 0 is a real parameter, so slicing it would introduce the mirror
// image of this bug; the cases at the bottom pin that down.

volatile char sink;

//===----------------------------------------------------------------------===//
// Static member operators: the object argument must not consume a parameter.
//===----------------------------------------------------------------------===//

// The reported shape.
struct Subscript {
  static string_view operator[](const string &key [[clang::lifetimebound]]) {
    return string_view(key);
  }
};

void subscript_dangles() {
  Subscript r;
  string_view sv;
  {
    string k("xxxx");
    sv = r[k]; // expected-warning {{local variable 'k' does not live long enough}}
  }            // expected-note {{destroyed here}}
  sink = sv.data()[0]; // expected-note {{later used here}}
}

// The `operator()` form, which was handled all along -- the two must agree.
struct Callable {
  static string_view operator()(const string &key [[clang::lifetimebound]]) {
    return string_view(key);
  }
};

void call_dangles() {
  Callable c;
  string_view sv;
  {
    string k("xxxx");
    sv = c(k); // expected-warning {{local variable 'k' does not live long enough}}
  }            // expected-note {{destroyed here}}
  sink = sv.data()[0]; // expected-note {{later used here}}
}

// Returning through the static subscript: the borrow must be attributed to the
// argument, not to the (also local) object.
string_view returns_temporary() {
  Subscript r;
  // expected-warning@+2 {{returning address of local temporary object}}
  // expected-warning@+1 {{stack memory associated with local temporary object is returned}}
  return r[string("xxxx")]; // expected-note {{returned here}}
}

// More than one parameter, with only the second bound: the shift would land on
// the first and report the wrong one.
struct TwoParams {
  static string_view operator[](const string &unbound [[clang::noescape]],
                                const string &key [[clang::lifetimebound]]) {
    return string_view(key);
  }
};

string_view second_param_bound(const string &live [[clang::noescape]]) {
  string tmp("yyyy");
  // expected-warning@+2 {{address of stack memory associated with local variable 'tmp' returned}}
  // expected-warning@+1 {{stack memory associated with local variable 'tmp' is returned}}
  return TwoParams{}[live, tmp]; // expected-note {{returned here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// The bound parameter receives storage that outlives the call.
string_view subscript_ok(const string &live [[clang::lifetimebound]]) {
  Subscript r;
  return r[live]; // no-warning
}

// Only the second parameter is bound, and it gets the long-lived argument; the
// short-lived `tmp` is not bound and must not be reported.
string_view second_param_bound_ok(const string &live [[clang::lifetimebound]]) {
  string tmp("yyyy");
  return TwoParams{}[tmp, live]; // no-warning
}

// A NON-static member operator, where argument 0 really is the object and must
// keep consuming the implicit object parameter.
struct NonStatic {
  string_view operator[](const string &key [[clang::lifetimebound]]) const {
    return string_view(key);
  }
};

string_view non_static_ok(const string &live [[clang::lifetimebound]]) {
  NonStatic r;
  return r[live]; // no-warning: `r` is the object, `live` outlives the return
}
