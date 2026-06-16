// DESC: `__real__`/`__imag__` (UO_Real/UO_Imag) fell into VisitUnaryOperator's
// default case and flowed no loan, so `&__real__ c` borrowed nothing -- a
// returned address of a local complex's component dangled silently. The operand's
// origin is now propagated so the borrow is tracked.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-return
volatile long sink;
__attribute__((noinline)) int *leak() {
  _Complex int c = 0;
  return &__real__ c; // returns &c.real -- a stack address
}
int main() {
  int *d = leak();
  volatile int junk[64];
  for (int i = 0; i < 64; ++i) junk[i] = i;
  (void)junk;
  sink = (long)d;
  *d = 42; // use-after-return (write)
  return 0;
}
