// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-assumed-invalidation -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// Invoking a callable can mutate whatever it captured by reference. The check is
// loan-based: a callable's value carries the loans of its by-reference captures
// (VisitLambdaExpr flows them into the closure; std::function construction flows
// them onward), so invalidating the callable argument's loans connects to a live
// borrow -- regardless of how the closure reached the call (directly, type-
// erased into a std::function, via a variable, or a generic helper). A
// [[clang::noescape]] parameter does not prevent the mutation.

const char *Long = "long enough heap-allocated backing string value here!!!!";

// A generic helper that receives a caller-supplied action and may invoke it.
// The hazard is at the *passing* call site (the helper may invoke the closure,
// mutating its by-reference captures), so the helper body need not invoke it
// here for the diagnostic to fire.
template <class Action> void withScope(Action action [[clang::noescape]]) {}
void run_fn(std::function<void()> a [[clang::noescape]]);

// (1) Lambda passed by value to a generic [[noescape]] callback.
void via_template_callback() {
  string text = Long;
  string_view tok = text; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  withScope([&text] { text.push_back('x'); }); // expected-note {{assumed to be invalidated by this operation}}
  (void)tok;
}

// (2) Lambda type-erased into a std::function parameter (inline).
void via_std_function_inline() {
  string text = Long;
  string_view tok = text; // expected-warning {{may be invalidated}}
  run_fn([&text] { text.push_back('x'); }); // expected-note {{assumed to be invalidated by this operation}}
  (void)tok;
}

// (3) Stored in a std::function variable first, then passed.
void via_std_function_variable() {
  string text = Long;
  string_view tok = text; // expected-warning {{may be invalidated}}
  std::function<void()> closure = [&text] { text.push_back('x'); }; // expected-warning {{may be invalidated}}
  run_fn(closure); // expected-note 2 {{assumed to be invalidated by this operation}}
  (void)tok;
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A by-value capture is an independent copy; invoking it cannot reallocate the
// caller's owner.
void by_value_capture() {
  string text = Long;
  string_view tok = text;
  withScope([text]() mutable { text.push_back('x'); }); // no-warning
  (void)tok;
}

// A callable that does not capture the borrowed owner is unrelated.
void unrelated_capture() {
  string text = Long;
  string other = Long;
  string_view tok = text;
  withScope([&other] { other.push_back('x'); }); // no-warning
  (void)tok;
}
