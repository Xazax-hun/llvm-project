// DESC: a C11 atomic builtin (`__c11_atomic_load`) lowers to an AtomicExpr AST
// node for which the fact generator had no handler, so the loaded pointer
// carried an empty origin and the borrow it held (`&x`) was silently dropped --
// a direct `return` of it escaped nothing. A generic catch-all (VisitExpr) now
// flags any origin-bearing expression the analysis does not model, so this and
// future unmodeled constructs surface instead of silently dropping a borrow.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-return
volatile long sink;
__attribute__((noinline)) int *f() {
  int x = 12345;
  _Atomic(int *) p = &x;
  return __c11_atomic_load(&p, __ATOMIC_RELAXED); // returns &x -- a stack address
}
int main() {
  int *d = f();
  volatile int junk[64];
  for (int i = 0; i < 64; ++i)
    junk[i] = i;
  (void)junk;
  sink = (long)d;
  return *d; // use-after-return
}
