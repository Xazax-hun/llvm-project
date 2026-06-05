// DESC: a pointer into a default-argument temporary outlives it
// EXPECT-ASAN: stack-use-after-scope
struct Big {
  int data[4];
};
__attribute__((noinline)) const int *f(const Big &b = Big{}) {
  return &b.data[0];
}
int main() {
  const int *p = f(); // the default temporary is destroyed after the call
  return *p;
}
