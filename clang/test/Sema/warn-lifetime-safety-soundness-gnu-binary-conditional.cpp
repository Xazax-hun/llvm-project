// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wno-gnu-conditional-omitted-operand -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;

// The GNU binary conditional `a ?: b` evaluates its common operand once, and every
// accessor for it -- getCond(), getTrueExpr() -- hands back the OpaqueValueExpr
// that STANDS FOR that operand rather than the operand itself. The CFG's
// temporary-destructor pass does not descend through an opaque value, so a
// temporary created in the common operand gets no cleanup element and no
// destructor.
//
// Expiry is this analysis's single trigger for reporting a use after a temporary
// dies, so with none emitted the borrow looks immortal and use-after-scope,
// return-stack-addr and dangling-global all go quiet together -- with no refusal
// to mark the gap. The same expression without the `?: "default"` is reported, and
// so is the full ternary.
//
// Refuse the construct until the CFG models it. The question is asked of the
// CONSTRUCT rather than of the loans on purpose: what is missing is a CFG element,
// and by the time the loans could be consulted the expiry that should have been
// generated is simply absent -- indistinguishable from a borrow that legitimately
// outlives the statement.
//
// Fixing the CFG is the better answer, and the obvious attempt is wrong: visiting
// the common operand in VisitConditionalOperatorForTemporaries double-counts
// whenever the temporary is already reachable, which it is when the conditional is
// class-typed. `A a = A() ?: A();` then destroys the common operand's temporary
// twice (one extra destructor in the join block, on every path).

volatile char sink;

string makeStr();

//===----------------------------------------------------------------------===//
// Refused: the common operand materializes a temporary.
//===----------------------------------------------------------------------===//

// The reported shape: use-after-scope.
void use_after_scope() {
  const char *s = makeStr().c_str() ?: "default"; // expected-warning {{common operand of this GNU conditional creates a temporary whose destruction is not modeled}}
  sink = s[0];
}

// The same gap silenced a dangling-global report.
const char *g;

void dangling_global() {
  g = makeStr().c_str() ?: "default"; // expected-warning {{common operand of this GNU conditional creates a temporary whose destruction is not modeled}}
}

// ...and a return of stack memory.
const char *return_stack_addr() {
  return makeStr().c_str() ?: "default"; // expected-warning {{common operand of this GNU conditional creates a temporary whose destruction is not modeled}}
}

// The temporary need not be the immediate operand; it just has to be in there.
void nested_temporary(bool c) {
  const char *s = (c ? makeStr().c_str() : nullptr) ?: "default"; // expected-warning {{common operand of this GNU conditional creates a temporary whose destruction is not modeled}}
  sink = s[0];
}

//===----------------------------------------------------------------------===//
// Must stay silent: no temporary in the common operand, so nothing is unmodeled.
//===----------------------------------------------------------------------===//

const char *pick(const char *p [[clang::lifetimebound]]);

void from_parameter(const char *p [[clang::noescape]]) {
  const char *s = p ?: "default"; // no-warning
  sink = s[0];
}

void from_call_result(const char *p [[clang::noescape]]) {
  const char *s = pick(p) ?: "default"; // no-warning
  sink = s[0];
}

// A named string outlives the statement, so its buffer is not a temporary.
void from_named(const string &s0 [[clang::noescape]]) {
  const char *s = s0.c_str() ?: "default"; // no-warning
  sink = s[0];
}

// A type that carries no borrow cannot dangle through the conditional.
int arithmetic(int a) {
  return a ?: 7; // no-warning
}

// The full ternary spelling was reported all along and must keep working: both
// arms are real expressions, so their temporaries are collected normally.
void full_ternary(bool c) {
  // expected-warning@+3 {{object backing the pointer will be destroyed}}
  // expected-warning@+2 {{local temporary object does not live long enough}}
  // expected-note@+1 {{destroyed here}}
  const char *s = c ? makeStr().c_str() : "default";
  sink = s[0]; // expected-note {{later used here}}
}
