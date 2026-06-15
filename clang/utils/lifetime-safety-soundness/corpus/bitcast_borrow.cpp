// DESC: __builtin_bit_cast of a pointer was unmodeled by VisitCastExpr (cast
// kind CK_LValueToRValueBitCast fell into the default case), so a borrow
// laundered through it was silently dropped -- an asymmetry against
// reinterpret_cast, which is caught. The bit-cast now propagates the borrow, so
// returning the bit-cast of a stack address is caught (return-stack-address).
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-return
volatile long sink;
__attribute__((noinline)) int *f() {
  int x = 13;
  return __builtin_bit_cast(int *, &x); // returns &x -- a stack address
}
int main() {
  int *p = f();
  sink = (long)p;
  return *p; // use-after-return
}
