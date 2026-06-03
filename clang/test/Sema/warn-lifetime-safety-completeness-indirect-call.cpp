// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-indirect-call -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-completeness -verify %s

// Calls whose callee cannot be resolved to a function (function pointers,
// member-function pointers) cannot carry lifetime annotations, so the analysis
// cannot model them. Results are discarded here to keep the focus on the
// indirect-call warning (an unused pointer result would also be "lost").

void use(int *);
int *direct();

using VoidFP = void (*)();
using IntPtrFP = int *(*)();

void via_function_pointer(VoidFP vfp, IntPtrFP ifp) {
  vfp(); // expected-warning {{call through a function pointer or otherwise unresolved callee is not modeled by lifetime safety analysis}}
  ifp(); // expected-warning {{call through a function pointer or otherwise unresolved callee is not modeled by lifetime safety analysis}}
}

void direct_call_ok() {
  direct(); // no-warning
}

struct S {
  void f();
};
typedef void (S::*PMF)();

void via_member_function_pointer(S s, PMF pmf) {
  (s.*pmf)(); // expected-warning {{call through a function pointer or otherwise unresolved callee is not modeled by lifetime safety analysis}}
}
