// RUN: %clang_cc1 -fsyntax-only -std=c++20 -flifetime-safety-inference -fexperimental-lifetime-safety-tu-analysis -Wlifetime-safety-soundness -verify %s

// Regression test: annotation inference (-flifetime-safety-inference) attaches an
// implicit [[clang::lifetimebound]] to a method's implicit object parameter by
// wrapping the method type in an AttributedType and rebuilding its
// TypeSourceInfo. For a method with a deduced ('auto'/'decltype(auto)') return
// type, the stored TypeSourceInfo keeps the written 'auto' while the method's
// type is the deduced type; the two diverge, which used to trip a TypeLocBuilder
// assertion ("mismatch between last type and new type's inner type") when
// inference ran in TU-end mode. The analysis must not crash and should still
// infer/diagnose normally.

struct Box {
  int val;
  // Deduced return type that resolves to a borrow of a member; inference wants
  // to mark the implicit object 'lifetimebound'.
  auto &get() { return val; } // expected-warning {{member function returning 'int &' is not annotated for lifetime safety}} \
                              // expected-warning {{implicit this in intra-TU function should be marked [[clang::lifetimebound]]}} \
                              // expected-note {{param returned here}}
};

// A 'decltype(auto)' form exercises the same deduced-return path.
struct Box2 {
  int val;
  decltype(auto) get() { return (val); } // expected-warning {{member function returning 'int &' is not annotated for lifetime safety}} \
                                          // expected-warning {{implicit this in intra-TU function should be marked [[clang::lifetimebound]]}} \
                                          // expected-note {{param returned here}}
};

const int &use(Box &b) { return b.get(); }   // expected-warning {{parameter that can hold a borrow is not annotated for lifetime safety}} \
                                              // expected-warning {{parameter in intra-TU function should be marked [[clang::lifetimebound]]}} \
                                              // expected-note {{param returned here}}
const int &use2(Box2 &b) { return b.get(); } // expected-warning {{parameter that can hold a borrow is not annotated for lifetime safety}} \
                                              // expected-warning {{parameter in intra-TU function should be marked [[clang::lifetimebound]]}} \
                                              // expected-note {{param returned here}}
