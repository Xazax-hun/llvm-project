// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -fexperimental-lifetime-safety-tu-analysis -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -fexperimental-lifetime-safety-tu-analysis -flifetime-safety-inference -verify %s

// TU-end analysis (-fexperimental-lifetime-safety-tu-analysis) discovers
// functions to analyze by walking the call graph in post order. The call graph
// reaches functions only through declaration traversal and the bodies of
// functions it has already added; it does not descend into statements such as a
// namespace-scope variable initializer. A lambda call operator defined in such
// an initializer was therefore never added to the graph and its body was never
// analyzed -- a silent coverage gap. A supplementary sweep now analyzes every
// callable the call graph missed, so the result matches the default (per-
// function) mode. All three RUN lines must produce identical diagnostics.

int *leaked = nullptr; // expected-note 2 {{this global dangles}}

// A lambda in a namespace-scope variable initializer whose body lets a stack
// address escape to a global. This was silent under TU mode.
int escaping = [] {
  int local = 0;
  leaked = &local; // expected-warning {{stack memory associated with local variable 'local' escapes to the global variable 'leaked'}}
  return 1;
}();

// A lambda nested inside another namespace-scope-initializer lambda is reached
// too (the supplementary sweep is recursive).
int nested = [] {
  auto inner = [] {
    int local = 0;
    leaked = &local; // expected-warning {{stack memory associated with local variable 'local' escapes to the global variable 'leaked'}}
    return 0;
  };
  return inner();
}();

// A clean lambda in a namespace-scope initializer stays silent (no false
// positive introduced by the new sweep).
int ok = [] {
  int local = 42;
  return local;
}();
