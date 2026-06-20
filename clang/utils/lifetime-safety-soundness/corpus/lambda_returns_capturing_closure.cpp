// DESC: a nested immediately-invoked lambda. The outer lambda captures `x` by
// reference and, when invoked, returns the inner closure -- which also borrows
// `x`. The inner closure escapes `make`, so invoking it after `make` returns
// reads dangling stack memory. It was silent: the outer operator()'s return type
// is a closure (not a pointer/reference/view), and the "member function
// returning a borrow must be annotated" check only looked at pointer-like return
// types -- so a borrow-holding closure return slipped, and the call boundary
// dropped the captured loan. Found by the 65th multi-agent bypass hunt (D).
// Closed by firing the unannotated-return check for any return type that holds a
// borrow (including a capturing closure), in the body of the outer lambda.
// EXPECT-ASAN: stack-use-after-return
#include <cstdio>

__attribute__((noinline)) auto make() {
  int x = 0xCAFE;
  return [&x] { return [&x] { return x; }; }(); // inner closure borrows make's x
}

int main() {
  auto inner = make();
  volatile int j[64];
  for (int i = 0; i < 64; ++i)
    j[i] = i; // clobber the reclaimed frame
  printf("%d\n", inner()); // read of dangling x
  return 0;
}
