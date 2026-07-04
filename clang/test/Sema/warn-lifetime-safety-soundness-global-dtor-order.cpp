// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-view-on-mutable-global -Wlifetime-safety-immortal-violation -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"

// A borrow of a global/static owner whose type has a non-trivial destructor is
// not immortal: the owner's buffer is freed at static destruction, and the
// destruction order across translation units is not something the
// intra-procedural analysis can reason about. When such a borrow escapes into
// other global/static storage, a longer-lived global holding it can read freed
// memory at teardown. This is flagged even when the global is `const` (a const
// owner cannot be mutated, but its buffer is still freed).

namespace std {}
using namespace std;

const string g_const_str;
string g_mut_str;

//===----------------------------------------------------------------------===//
// Escape into global storage: flagged.
//===----------------------------------------------------------------------===//

string_view g_view; // expected-warning {{borrows from a global or static object with a non-trivial destructor and escapes to global or static storage}}

void store_const_to_global() {
  g_view = g_const_str;
}

struct Holder {
  string_view v;
};
Holder g_holder; // expected-warning {{borrows from a global or static object with a non-trivial destructor and escapes to global or static storage}}

void store_const_to_global_field() {
  g_holder.v = g_const_str;
}

//===----------------------------------------------------------------------===//
// [[clang::lifetime_immortal]] body verifier: a view of such a global is not
// immortal storage.
//===----------------------------------------------------------------------===//

[[clang::lifetime_immortal]] string_view immortal_const() { // expected-warning {{returns a borrow of an object the analysis cannot prove is immortal}}
  return g_const_str;
}

//===----------------------------------------------------------------------===//
// Negatives: genuinely safe cases stay clean.
//===----------------------------------------------------------------------===//

// A borrow of a const global owner used LOCALLY (not escaping to global storage)
// is safe: the const global outlives the function.
volatile char sink;
void local_use_ok() {
  string_view v = g_const_str;
  sink = v.data()[0]; // no-warning
}

// A stable scalar global's address stored into a global pointer is safe: the
// address never dangles and there is no owner buffer to free.
int g_int = 7;
int *g_ptr;
void scalar_ok() {
  g_ptr = &g_int; // no-warning
}

// A string literal is genuinely immortal.
const char *g_lit;
void literal_ok() {
  g_lit = "hello"; // no-warning
}
