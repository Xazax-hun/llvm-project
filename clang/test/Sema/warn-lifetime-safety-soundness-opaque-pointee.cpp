// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

volatile char sink;

// Assumed invalidation asks whether a callee can reach a mutable owner through a
// parameter. Answering that from the parameter's pointee type only works when the
// pointee is KNOWN. Two pointees are opaque, and neither can be shown to be
// owner-free, so neither may be assumed to be:
//
//   - `void *`, the C-interop "userdata" idiom: the callee casts it back to the
//     real type and can mutate an owner through it, while the signature reveals
//     nothing.
//   - an INCOMPLETE record, the opaque-handle idiom. This one is also
//     order-dependent: the type may be completed later in the TU, so whether the
//     hazard was visible otherwise depended on where the analysis ran.
//
// Note both annotations below are TRUTHFUL -- the parameter really does not
// escape -- so nothing else flags these.

//===----------------------------------------------------------------------===//
// void * parameter.
//===----------------------------------------------------------------------===//

static void clear_through_void(void *p [[clang::noescape]]) {
  *static_cast<string *>(p) = string(); // frees the old buffer
}

void borrow_across_void_call() {
  string s = "a string long enough to be heap allocated for sure!!";
  string_view v = s; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  clear_through_void(&s); // expected-note {{assumed to be invalidated by this operation}}
  sink = *v.data();
}

//===----------------------------------------------------------------------===//
// Incomplete pointee.
//===----------------------------------------------------------------------===//

struct Session; // opaque at the point `run` is analyzed
void session_reset(Session *s [[clang::noescape]]);
const char *session_text(const Session *s [[clang::lifetimebound]]);

// Anchored at the parameter, which is the borrowed storage here: the loan is the
// caller-scope placeholder for `s`, so there is no borrow expression to point at.
// The accessor takes a `const Session *`, so only the non-const call is treated as
// possibly mutating.
void run(Session *s [[clang::noescape]]) { // expected-warning {{parameter may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  const char *p = session_text(s);
  session_reset(s); // expected-note {{assumed to be invalidated by this operation}}
  sink = *p;
}

// Completed only afterwards -- which is exactly why the check cannot depend on
// completeness at the point of analysis.
struct Session {
  string t;
};

//===----------------------------------------------------------------------===//
// Negatives: an opaque parameter is not by itself a diagnostic. The invalidation
// is only reported when a borrow is actually live across the call.
//===----------------------------------------------------------------------===//

static void consume(void *p [[clang::noescape]]) { (void)p; }

void no_live_borrow() {
  string s = "a string long enough to be heap allocated for sure!!";
  consume(&s);       // no-warning: nothing borrowed across this call
  sink = *s.data();  // the borrow is taken afterwards
}

void handle_passthrough(Session *s [[clang::noescape]]) {
  session_reset(s); // no-warning: no borrow outstanding
}

// A const pointee still cannot be mutated through.
static void peek(const void *p [[clang::noescape]]) { (void)p; }
void const_void_is_clean() {
  string s = "a string long enough to be heap allocated for sure!!";
  string_view v = s;
  peek(&s); // no-warning: const
  sink = *v.data();
}
