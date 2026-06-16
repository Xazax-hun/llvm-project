// DESC: a store into a member of `this` through a `*&` round-trip wrapping this
// (`(*&*this).p = &local`) was not recognized as a this-member store -- isThisExpr
// saw through `*this` and base-casts but not a deref-of-address-of round-trip --
// so the borrow routed to a fresh disconnected origin and was dropped, leaving the
// dangling field read silent. isThisExpr now collapses a `*&X` round-trip.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-scope
struct Holder {
  const int *p = nullptr;
  __attribute__((noinline)) int use() {
    { int local = 7; (*&*this).p = &local; } // store via (*&*this).p -- dropped
    { volatile int junk[64]; for (int i = 0; i < 64; ++i) junk[i] = i; (void)junk; }
    return *p;                               // dangling read
  }
};
volatile int sink;
int main() { Holder h; sink = h.use(); return 0; }
