// DESC: a static pointer holds a borrow to a local across calls
// EXPECT-ASAN: stack-use-after-return
__attribute__((noinline)) int *stash(int v) {
  static int *s = nullptr;
  int *prev = s;
  int x = v;
  s = &x;      // s points at this call's x
  return prev; // returns the previous call's (now dead) x
}
int main() {
  stash(1);
  return *stash(2);
}
