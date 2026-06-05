// DESC: a borrow to a local escapes by being stored into a global
// EXPECT-ASAN: stack-use-after-return
int *g_p;
__attribute__((noinline)) void store() {
  int x = 5;
  g_p = &x;
}
int main() {
  store();
  return *g_p;
}
