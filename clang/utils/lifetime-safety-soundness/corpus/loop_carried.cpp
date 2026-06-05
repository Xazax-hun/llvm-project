// DESC: a pointer holds a borrow from a previous loop iteration's local
// EXPECT-ASAN: stack-use-after-scope
__attribute__((noinline)) int read(int *p) { return *p; }
int main() {
  int *p = nullptr;
  for (int i = 0; i < 2; ++i) {
    if (i == 1)
      return read(p); // p points at iteration 0's (now dead) x
    int x = 100 + i;
    p = &x;
  }
  return 0;
}
