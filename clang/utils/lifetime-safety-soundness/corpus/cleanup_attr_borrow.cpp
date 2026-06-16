// DESC: a [[gsl::Pointer]] variable with __attribute__((cleanup(fn))) is cleaned
// up at scope exit (fn(&var) is called) in reverse construction order. Declared
// before the local it borrows, the cleanup runs after that local is destroyed
// and the callback reads the dangling borrow. FactsGenerator only handled the
// non-trivial-destructor-as-use path (CFGLifetimeEnds) and ignored the
// CFGCleanupFunction element, so the cleanup call was not modeled as a use and
// the borrow was dropped before the local's expiry -> silent. Now the cleanup
// call is modeled as a use of the variable.
// EXPECT-ASAN: stack-use-after-scope
#include <cstdio>
struct [[gsl::Pointer(int)]] View { const int *p; };
__attribute__((noinline)) void cleanup_view(View *v) {
  if (v->p) { volatile int t = *v->p; printf("%d\n", t); }
}
__attribute__((noinline)) void f() {
  View g __attribute__((cleanup(cleanup_view))) = View{nullptr}; // cleaned up LAST
  int local = 4242;                                              // destroyed FIRST
  g = View{&local};   // g borrows local; cleanup reads it after it is gone
}
int main() {
  f();
  return 0;
}
