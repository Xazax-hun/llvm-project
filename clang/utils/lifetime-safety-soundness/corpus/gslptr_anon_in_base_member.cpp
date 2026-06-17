// DESC: a borrow stored into a member that is reached through BOTH an anonymous
// struct AND a [[gsl::Pointer]] base class at once (`struct [[gsl::Pointer]]
// Base { struct { int* p; }; }; struct D : Base {int extra;}; bug(D& d){ d.p =
// &local; ... *d.p }`). The round-48 (anon) and round-49a (base) fixes each
// recognized one shape; combined, the member's declaring class is the anonymous
// struct (not gsl) reached through a non-gsl derived, so both branches missed and
// the store was dropped -- silent in the bug fn. The merge now walks the member's
// declaring record outward through enclosing anonymous records to find the
// enclosing gsl::Pointer.
// EXPECT-ASAN: stack-use-after-scope
#include <cstdio>
struct [[gsl::Pointer]] Base { struct { int *p; }; };
struct D : Base { int extra; };
volatile int sink;
__attribute__((noinline)) void bug(D &d) {
  { int local = 0xABCD; d.p = &local; } // local dies
  sink = *d.p;                           // dangling read
}
int main() {
  D d;
  d.p = nullptr;
  bug(d);
  printf("%d\n", sink);
  return 0;
}
