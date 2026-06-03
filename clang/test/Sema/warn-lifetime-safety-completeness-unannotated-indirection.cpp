// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-unannotated-indirection -verify %s

// The "safe programming model" requires every indirection (a parameter that can
// hold a borrow: raw pointer/reference or a gsl::Pointer) to be annotated. This
// is enforced both at the function definition (its own parameters) and at call
// sites (arguments bound to an unannotated callee parameter).

struct [[gsl::Pointer]] View {
  const int *p;
};

//===----------------------------------------------------------------------===//
// Definition site: the function's own parameters.
//===----------------------------------------------------------------------===//

void def_unannotated(int *p, int &r, View v) { // expected-warning 3 {{parameter that can hold a borrow is not annotated for lifetime safety}}
  (void)p;
  (void)r;
  (void)v;
}

const int *def_annotated(int *p [[clang::lifetimebound]],
                         int &r [[clang::noescape]],
                         View v [[clang::lifetimebound]]) { // no-warning
  (void)r;
  (void)v;
  return p;
}

void def_by_value(int x, View *pv) { // expected-warning {{parameter that can hold a borrow is not annotated for lifetime safety}}
  // 'x' is not an indirection; only 'pv' (a pointer) is flagged.
  (void)x;
  (void)pv;
}

//===----------------------------------------------------------------------===//
// Call site: arguments bound to an unannotated callee parameter.
//===----------------------------------------------------------------------===//

void sink(int *p);
void sink_noescape(int *p [[clang::noescape]]);
const int *sink_lifetimebound(int *p [[clang::lifetimebound]]);

void caller() {
  int x;
  sink(&x);              // expected-warning {{argument is bound to a parameter that can hold a borrow but is not annotated for lifetime safety}}
  sink_noescape(&x);     // no-warning
  sink_lifetimebound(&x); // no-warning
}
