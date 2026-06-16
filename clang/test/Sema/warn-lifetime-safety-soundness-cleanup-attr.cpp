// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A variable with `__attribute__((cleanup(fn)))` has `fn(&var)` called at scope
// exit, in reverse construction order alongside destructors. That callback may
// read a borrow the variable holds. Like a non-trivial destructor, the cleanup
// call is modeled as a use of the variable, so a [[gsl::Pointer]] cleanup
// variable declared before the local it borrows (and thus cleaned up after that
// local is destroyed) reports a use-after-scope.

struct [[gsl::Pointer(int)]] View {
  const int *p;
};
void cleanup_view(View *v); // out-of-line: may read v->p

void declared_before() {
  View g __attribute__((cleanup(cleanup_view))) = View{nullptr}; // cleaned up LAST
  // expected-note@-1 {{later used here}}
  int local = 7;
  g = View{&local}; // expected-warning {{local variable 'local' does not live long enough}}
} // expected-note {{destroyed here}}

// Negative: cleanup variable declared AFTER the local is cleaned up FIRST, so the
// callback reads a still-live local -- no error.
void declared_after() {
  int local = 7;
  View g __attribute__((cleanup(cleanup_view))) = View{nullptr};
  g = View{&local}; // no-warning
}
