// DESC: a [[gsl::Pointer]] whose borrow-holding member lives in an ANONYMOUS
// struct defeated the round-26/28 view-member-store merge (gated on the assigned
// member's immediate base being a gsl::Pointer -- but `v.p`'s base is the unnamed
// anonymous-struct subobject, whose type is not a gsl::Pointer). So `v.p = &local`
// dropped the borrow from the view's origin and the later dangling read was
// silent (the bug-containing function emitted nothing). The merge gate now peels
// anonymous-record bases to reach the real enclosing gsl::Pointer object.
// EXPECT-ASAN: stack-use-after-scope
#include <cstdio>
struct [[gsl::Pointer]] V {
  struct { int *p; };   // anonymous struct holds the borrow
  V() { p = nullptr; }
};
__attribute__((noinline)) int use(V v) {
  { volatile int local = 0xC0FFEE; v.p = (int *)&local; } // local dies here
  return *v.p;                                            // dangling read
}
int main() {
  V v;
  printf("%d\n", use(v));
  return 0;
}
