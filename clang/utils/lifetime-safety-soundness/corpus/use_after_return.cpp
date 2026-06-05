// DESC: function returns the address of a local
// EXPECT-ASAN: stack-use-after-return
__attribute__((noinline)) int *make() {
  int x = 123;
  return &x;
}
int main() { return *make(); }
