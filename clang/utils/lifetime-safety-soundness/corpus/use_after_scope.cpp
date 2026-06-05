// DESC: pointer to a local outliving its enclosing scope
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *p;
  {
    int x = 42;
    p = &x;
  }
  return *p;
}
