// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

// The soundness group is the completeness group for the safe programming model.
// Besides the modeling-gap warnings, it must also include (a) the strict
// bug-detection warnings and (b) the annotation-validation warnings -- an
// annotation the body does not honor is a hole callers trust. This RUN line
// enables ONLY -Wlifetime-safety-soundness (not -strict / -validations) and
// expects the warnings to fire anyway.

// (a) Strict bug detection is part of soundness: a plain use-after-scope fires
// under -Wlifetime-safety-soundness alone.
const int *use_after_scope() {
  const int *p;
  {
    int x = 5;
    p = &x; // expected-warning {{local variable 'x' does not live long enough}}
  } // expected-note {{destroyed here}}
  return p; // expected-note {{later used here}}
}

// (b) Annotation validation is part of soundness: a [[clang::noescape]]
// parameter that escapes via return is a violated contract. (The return is
// also not bound to the [[clang::lifetimebound]] 'a', so that contract cannot
// be verified either.)
const int &noescape_lie(
    const int &a [[clang::lifetimebound]], // expected-warning {{could not verify that the return value can be lifetime bound to 'a'}}
    const int &b [[clang::noescape]]) {    // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return b;                                // expected-note {{param returned here}}
}

// A [[clang::lifetimebound]] whose binding cannot be confirmed is a violated
// contract under soundness too.
const int &lifetimebound_unverifiable(
    const int &a [[clang::lifetimebound]]) { // expected-warning {{could not verify that the return value can be lifetime bound to 'a'}}
  static int s = 0;
  return s;
}
