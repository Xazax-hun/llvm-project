// DESC: a [[gsl::Pointer]] aggregate with BOTH a pointer member and a const&
// REFERENCE member. Aggregate-init's per-member loan merge (round-29) skipped
// the reference member -- its initializer is the referent glvalue (depth 0), so
// getRValueOrigins peeled it away rather than treating it as the depth-1 borrow
// `&local`. The dropped reference borrow was masked by the sibling pointer
// member's long-lived (global) loan, suppressing lost-loan -> silent: `&m.r`
// (== &local) was returned as if it held the global loan. A reference-member
// initializer now contributes the borrow of its bound lvalue to the leaf origin.
// EXPECT-ASAN: stack-use-after-return
#include <cstdio>
struct [[gsl::Pointer(int)]] Mixed { const int *p; const int &r; };
static int g_long = 100;
__attribute__((noinline)) const int *dangle() {
  const int *held = nullptr;
  { int local = 0xABCD; Mixed m = {&g_long, local}; held = &m.r; } // local dies
  return held; // really &local
}
int main() {
  const int *h = dangle();
  printf("%d\n", *h); // reads freed stack slot
  return 0;
}
