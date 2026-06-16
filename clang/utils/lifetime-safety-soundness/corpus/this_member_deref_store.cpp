// DESC: a store into a member of `this` spelled `(*this).p = &local` (a deref of
// this) was not recognized as a this-member store -- the recognition matched only
// a direct CXXThisExpr base, not `*this` -- so the borrow routed to a fresh
// disconnected origin and was dropped, while the field's real origin kept its
// uninitialized sentinel; the dangling field read was silent. `this`-member
// recognition now accepts `*this` (and base-casts of this) alike.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-scope
struct Holder {
  const int *p = nullptr;
  __attribute__((noinline)) int use() {
    { int local = 7; (*this).p = &local; }   // store via (*this).p -- dropped
    { volatile int junk[64]; for (int i = 0; i < 64; ++i) junk[i] = i; (void)junk; }
    return *p;                                // dangling read
  }
};
volatile int sink;
int main() { Holder h; sink = h.use(); return 0; }
