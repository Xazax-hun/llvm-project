// DESC: a borrow to a local escapes through a pointer out-parameter
// EXPECT-ASAN: stack-use-after-return
__attribute__((noinline)) void set(int **out) {
  int x = 5;
  *out = &x;
}
int main() {
  int *p = nullptr;
  set(&p);
  return *p;
}
