// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wno-missing-noescape -Wlifetime-safety-soundness -verify %s

// The soundness group includes -Wmissing-noescape: an override that drops a
// '[[clang::noescape]]' the overridden method advertises is a soundness hole,
// because callers dispatching through the base class assume the parameter does
// not escape and pass short-lived borrows. (The second RUN line shows the
// soundness group re-enables the warning even after -Wno-missing-noescape.)

struct Base {
  virtual void sink(const int *p [[clang::noescape]]); // expected-note {{parameter of overridden method is annotated with __attribute__((noescape))}}
};

struct Derived : Base {
  void sink(const int *p) override; // expected-warning {{parameter of overriding method should be annotated with __attribute__((noescape))}}
};

// Keeping the annotation is fine.
struct DerivedOK : Base {
  void sink(const int *p [[clang::noescape]]) override; // no-warning
};
