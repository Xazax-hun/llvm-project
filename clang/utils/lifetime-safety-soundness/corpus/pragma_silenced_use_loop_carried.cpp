// DESC: a `#pragma clang diagnostic ignored` around one use hid a DIFFERENT,
// unsilenced use that runs on the next loop iteration. Which uses come "after"
// the expiry was decided by comparing source locations, and the loop-carried use
// is written BEFORE the end of `x`'s scope.
// EXPECT-ASAN: stack-use-after-scope
volatile int sink;
int main() {
  int *p = nullptr;
  for (int i = 0; i < 2; ++i) {
    if (p)
      sink += *p; // iteration 2 reads the previous iteration's `x`
    int x = i + 1;
    p = &x;
  }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-use-after-scope"
  sink += *p;
#pragma clang diagnostic pop
  return sink == 12345;
}
