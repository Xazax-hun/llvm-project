// DESC: a store into an array-of-pointers element whose subscript BASE selects
// among arrays -- `(c ? a : b)[i] = &local` -- routed the borrow to the transient
// element-origin of the conditional, which a later read `(c ? a : b)[i]` of the
// real arrays never re-resolves to, so the borrow was dropped; with the arrays
// uninitialized the lost-loan backstop was masked by the Uninitialized sentinel.
// The unroutable store is now rejected (a selecting array base is not stable
// storage). This is the array-subscript sibling of the scalar `(c?p:q)=...` case.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-scope
volatile int sink;

__attribute__((noinline)) int f(bool c) {
  int *a[4];
  int *b[4];
  {
    int local = 12345;
    (c ? a : b)[2] = &local; // selecting-base store: dropped
  }                          // local dies -> (c?a:b)[2] dangles
  // clobber the freed stack slot so the stale read is observable
  {
    volatile int junk[16];
    for (int i = 0; i < 16; ++i)
      junk[i] = 0xbeef;
    (void)junk;
  }
  return *((c ? a : b)[2]); // dangling read
}

int main() { return f(true); }
