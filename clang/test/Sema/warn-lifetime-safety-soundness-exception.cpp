// RUN: %clang_cc1 -fsyntax-only -fcxx-exceptions -Wlifetime-safety-exception -verify %s
// RUN: %clang_cc1 -fsyntax-only -fcxx-exceptions -Wlifetime-safety-soundness -verify %s
// RUN: %clang_cc1 -fsyntax-only -fcxx-exceptions -fexperimental-lifetime-safety-tu-analysis -Wlifetime-safety-soundness -verify %s

// Exception control flow (stack unwinding, running destructors and resuming in
// a handler) is not modeled by the lifetime safety analysis, so a borrow that
// dangles only along an exception path can be missed. Under the "safe
// programming model" every such construct is surfaced as an unsupported
// construct.

void mightThrow();

// A real use-after-scope that only manifests on the exception path: when the
// 'throw' unwinds, 'x' is destroyed, leaving 'p' dangling for the read after
// the try/catch. The analysis does not model this, but the 'throw' and 'try'
// are flagged.
int throw_dangles() {
  int *p = nullptr;
  try {                  // expected-warning {{exception control flow is not modeled}}
    int x = 5;
    p = &x;
    throw 1;             // expected-warning {{exception control flow is not modeled}}
  } catch (...) {
  }
  return *p;
}

// A 'try' whose body only calls a potentially-throwing function has no explicit
// 'throw' and produces no CFG exception edges, but is still flagged.
void try_around_call() {
  try {                  // expected-warning {{exception control flow is not modeled}}
    mightThrow();
  } catch (const int &e) {
    (void)e;
  }
}

// A bare 'throw' outside any 'try'.
void bare_throw() {
  throw 42;              // expected-warning {{exception control flow is not modeled}}
}

// Nested try statements are each flagged.
void nested_try() {
  try {                  // expected-warning {{exception control flow is not modeled}}
    try {                // expected-warning {{exception control flow is not modeled}}
      mightThrow();
    } catch (...) {
    }
  } catch (...) {
  }
}

// Code with no exception construct stays silent.
void no_exceptions() {
  int x = 0;
  int *p = &x;
  (void)p;
}

// A throw inside a nested lambda belongs to the lambda's own analysis, not the
// enclosing function's body scan, so the enclosing function is silent here.
void lambda_throw_not_counted() {
  auto f = [] {
    throw 1;             // expected-warning {{exception control flow is not modeled}}
  };
  (void)f;
}

// Per-construct opt-out: an explicit '#pragma clang diagnostic ignored' silences
// the warning for one region.
void opt_out() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-exception"
  try {                  // no-warning
    mightThrow();
  } catch (...) {
  }
#pragma clang diagnostic pop
}
