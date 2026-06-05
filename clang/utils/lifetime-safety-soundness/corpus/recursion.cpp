// DESC: a recursive function returns the address of one of its locals
// EXPECT-ASAN: stack-use-after-return
__attribute__((noinline)) int *rec(int n) {
  int x = n;
  if (n == 0)
    return &x;
  return rec(n - 1);
}
int main() { return *rec(3); }
