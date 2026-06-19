// RUN: %clang_cc1 -fsyntax-only -fobjc-exceptions -Wlifetime-safety-exception -verify %s
// RUN: %clang_cc1 -fsyntax-only -fobjc-exceptions -Wlifetime-safety-soundness -verify %s

// Objective-C exception control flow (@try/@catch/@throw) is not modeled by the
// lifetime safety analysis, just like C++ try/catch/throw. The handler resumes
// after the stack has unwound, an edge the CFG does not carry, so a borrow that
// dangles only along the exception path can be missed. Under the "safe
// programming model" every such construct is surfaced.

void mightThrow();

// A real use-after-scope that only manifests on the exception path: when the
// '@throw' unwinds, 'x' is destroyed, leaving 'p' dangling for the read in the
// handler. The analysis does not model this, but the '@throw' and '@try' are
// flagged. (This was a soundness bypass: the exception backstop was
// CXXTryStmt/CXXThrowExpr-only and missed the Objective-C spelling.)
int throw_dangles() {
  int *p = nullptr;
  @try {                 // expected-warning {{exception control flow is not modeled}}
    int x = 5;
    p = &x;
    @throw @"boom";      // expected-warning {{exception control flow is not modeled}}
  } @catch (...) {
    return *p;
  }
  return *p;
}

// An '@try' whose body only calls a potentially-throwing function has no
// explicit '@throw' but is still flagged.
void try_around_call() {
  @try {                 // expected-warning {{exception control flow is not modeled}}
    mightThrow();
  } @catch (id e) {
    (void)e;
  }
}

// A bare '@throw' outside any '@try'.
void bare_throw() {
  @throw @"oops";        // expected-warning {{exception control flow is not modeled}}
}

// Nested '@try' statements are each flagged.
void nested_try() {
  @try {                 // expected-warning {{exception control flow is not modeled}}
    @try {               // expected-warning {{exception control flow is not modeled}}
      mightThrow();
    } @catch (...) {
    }
  } @catch (...) {
  }
}

// Code with no exception construct stays silent.
void no_exceptions() {
  int x = 0;
  int *p = &x;
  (void)p;
}
