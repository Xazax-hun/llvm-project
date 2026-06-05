// DESC: returning the address of a lifetime-extended temporary (local scope)
// EXPECT-ASAN: stack-use-after-return
__attribute__((noinline)) const int *f() {
  const int &r = 5; // lifetime-extended temporary, dies when f returns
  return &r;
}
int main() { return *f(); }
