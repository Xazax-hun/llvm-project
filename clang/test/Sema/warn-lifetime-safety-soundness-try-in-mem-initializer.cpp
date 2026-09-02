// RUN: %clang_cc1 -fsyntax-only -std=c++20 -fcxx-exceptions -fexceptions -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;

// Exception control flow is refused, not modeled: a handler resumes after the
// stack has unwound, an edge the CFG does not carry, so a borrow that dangles
// only along that path would be missed. Nothing inside a try/catch is analyzed
// otherwise -- suppressing this refusal makes a plain use-after-free written in
// a catch block disappear -- so the refusal is the whole coverage there.
//
// It is an AST walk, and it was rooted at the function BODY alone. A
// constructor's MEM-INITIALIZERS are not part of its body, so a `try` written
// there (inside a statement-expression initializing a member) was not reachable
// from the walk's root and went unrefused -- while the identical `try` one line
// further down, in the body, was refused. runPreScan already seeded both the body
// and the initializers; this now does too.

volatile int sink;

int mayThrow();

// The reported shape: `try` inside a mem-initializer.
struct InInitializer {
  int a;
  InInitializer()
      : a(({
          int r = 0;
          try { // expected-warning {{exception control flow is not modeled}}
            r = mayThrow();
          } catch (...) {
            r = 1;
          }
          r;
        })) {}
};

// A base-class initializer is a mem-initializer too.
struct BaseWithInt {
  int b;
  BaseWithInt(int v) : b(v) {}
};

struct InBaseInitializer : BaseWithInt {
  InBaseInitializer()
      : BaseWithInt(({
          int r = 0;
          try { // expected-warning {{exception control flow is not modeled}}
            r = mayThrow();
          } catch (...) {
            r = 2;
          }
          r;
        })) {}
};

// The body case, which was refused all along -- the two must agree.
struct InBody {
  int d;
  InBody() {
    try { // expected-warning {{exception control flow is not modeled}}
      d = mayThrow();
    } catch (...) {
      d = 4;
    }
  }
};

// A plain function, for the same reason.
void in_function() {
  int r = 0;
  try { // expected-warning {{exception control flow is not modeled}}
    r = mayThrow();
  } catch (...) {
    r = 5;
  }
  sink = r;
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A mem-initializer with no exception construct.
struct PlainInitializer {
  int a;
  PlainInitializer() : a(({ int r = 7; r; })) {} // no-warning
};

// A lambda in a mem-initializer is a separate function, analyzed on its own; the
// walk must not descend into it and report here.
struct LambdaInInitializer {
  int a;
  LambdaInInitializer() : a([] { return 9; }()) {} // no-warning
};
