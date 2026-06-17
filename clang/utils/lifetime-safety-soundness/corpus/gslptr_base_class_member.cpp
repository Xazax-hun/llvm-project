// DESC: a borrow stored into a member of a [[gsl::Pointer]] BASE CLASS, through a
// non-gsl most-derived receiver passed by reference (`struct Derived : ViewBase
// {int extra;}; bug(Derived& d) { d.p = &local; ... *d.p }`). The view-member-
// store merge (rounds 26/28) was gated on the receiver's own static type being a
// gsl::Pointer; Derived is plain, so the merge was skipped, the borrow dropped,
// and the later read silent in the bug function (no local decl -> no
// unknown-ownership backstop). The merge now also recognizes a member declared
// in a gsl::Pointer base subobject.
// EXPECT-ASAN: stack-use-after-scope
#include <cstdio>
struct [[gsl::Pointer]] ViewBase { const int *p; };
struct Derived : ViewBase { int extra; };
volatile int sink;
__attribute__((noinline)) void bug(Derived &d) {
  { int local = 0xABCD; d.p = &local; } // local dies here
  sink = *d.p;                           // dangling read
}
int main() {
  Derived d;
  d.p = nullptr;
  bug(d);
  printf("%d\n", sink);
  return 0;
}
