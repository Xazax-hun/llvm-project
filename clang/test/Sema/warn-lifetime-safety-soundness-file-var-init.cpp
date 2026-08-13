// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s
//
// Also covered in TU mode, but that mode additionally sweeps the test header's
// implicit string_view::operator= (a pre-existing report unrelated to this
// file), so it is not -verify'd here.
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -fexperimental-lifetime-safety-tu-analysis %s 2>&1 | FileCheck %s
// CHECK: warn-lifetime-safety-soundness-file-var-init.cpp:{{.*}}borrows from a mutable global

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A namespace-scope variable's dynamic initializer is code, but no entry point
// used to reach it: the per-declaration path runs when a FUNCTION scope is
// popped, and TU mode walks callables and the call graph -- a file-scope
// VarDecl is neither, and CallGraph does not descend into initializer
// statements. A borrow stored by such an initializer was therefore never
// analyzed: not refused, simply invisible.
//
// The bug this hides is a cross-global lifetime error. Initialization order
// within a TU is declaration order, so destruction order is the reverse: a
// variable declared FIRST is destroyed LAST, and can read a borrow of one
// declared later after that one is already gone.

volatile char sink;

string g_mut = "a mutable global string";

//===----------------------------------------------------------------------===//
// A store performed by a namespace-scope initializer is analyzed.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] Watch {
  string_view v;
};
// The escape is reported against the borrow HOLDER's declaration, which is where
// the dangling value comes to rest.
Watch g_watch; // expected-warning {{borrows from a mutable global or static object}}

// The comma operator makes the initializer a statement sequence whose real work
// is the store. Nothing in it is a callable, so nothing used to analyze it.
// expected-warning@+1 {{cannot track global variable 'g_watch' here}}
static int g_wire = (g_watch.v = g_mut, 0);

// A borrow taken directly in the initializer, likewise.
Watch g_direct; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track global variable 'g_direct' here}}
static int g_wire2 = (g_direct.v = g_mut, 0);

//===----------------------------------------------------------------------===//
// Nested contexts are reached too: namespaces, extern "C", and the out-of-class
// initializer of a static data member.
//===----------------------------------------------------------------------===//

namespace app {
Watch g_nested; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track global variable 'g_nested' here}}
static int wire = (g_nested.v = g_mut, 0);
} // namespace app

namespace outer { namespace inner {
Watch g_deep; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track global variable 'g_deep' here}}
static int wire = (g_deep.v = g_mut, 0);
}} // namespace outer::inner

extern "C" {
Watch g_extern_c; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track global variable 'g_extern_c' here}}
static int g_wire3 = (g_extern_c.v = g_mut, 0);
}

struct Holder { static Watch s_watch; };
Watch Holder::s_watch; // expected-warning {{borrows from a mutable global or static object}}
// expected-warning@+1 {{cannot track static variable 's_watch' here}}
static int g_wire4 = (Holder::s_watch.v = g_mut, 0);

//===----------------------------------------------------------------------===//
// Negatives: ordinary global initialization stays clean.
//===----------------------------------------------------------------------===//

// Owners initialized by value borrow nothing.
string g_name = "an ordinary global string, no borrow involved here";
int g_count = 42;

// A string literal has static storage duration and is genuinely immortal.
const char *g_literal = "literals never dangle";

// A constant initializer performs no runtime store at all.
constexpr int g_const = 7;

// An aggregate of owners.
struct Cfg { string name; int n; };
Cfg g_cfg{"config", 3};
