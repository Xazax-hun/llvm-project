// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// An argument passed through the C variadic ellipsis (`...`) has no declared
// parameter, so it cannot carry a lifetime annotation (lifetimebound / noescape
// / lifetime_capture_by) and the analysis cannot model where the callee stores
// it. A borrow passed this way would escape silently. Such an argument is now
// rejected like an unannotated indirection parameter.

extern "C" void vstore(int n, ...);

void variadic_addressof() {
  int local = 0;
  vstore(1, &local); // expected-warning {{argument is bound to a parameter that can hold a borrow but is not annotated for lifetime safety}}
}

void variadic_local_ptr() {
  int local = 0;
  int *p = &local;
  vstore(1, p); // expected-warning {{argument is bound to a parameter that can hold a borrow but is not annotated for lifetime safety}}
}

int g_int = 0;
struct [[gsl::Pointer]] View {
  const int *p;
};
void variadic_view() {
  View v{&g_int};
  vstore(1, v); // expected-warning {{argument is bound to a parameter that can hold a borrow but is not annotated for lifetime safety}}
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A non-borrow variadic argument is fine.
void variadic_value() {
  int x = 5;
  vstore(1, x);    // no-warning
  vstore(2, 1, 2); // no-warning
}

// No variadic argument at all.
void declared_only() {
  vstore(42); // no-warning
}
