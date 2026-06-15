// DESC: a borrow stored into and read from an `_Atomic(T*)` was silently dropped
// -- the analysis did not see through the AtomicType (so the atomic carried no
// origin) and the atomic wrap/unwrap casts (CK_NonAtomicToAtomic /
// CK_AtomicToNonAtomic) were unmodeled. Seeing through _Atomic plus modeling the
// casts propagates the borrow, so returning a stack address laundered through an
// atomic is caught (return-stack-address).
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-return
volatile long sink;
__attribute__((noinline)) const int *f() {
  int x = 13;
  _Atomic(int *) a = &x;
  return a; // returns &x -- a stack address
}
int main() {
  const int *p = f();
  sink = (long)p;
  return *p; // use-after-return
}
