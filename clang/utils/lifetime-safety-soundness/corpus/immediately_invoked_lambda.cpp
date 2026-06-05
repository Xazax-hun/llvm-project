// DESC: a dangling borrow read inside an immediately-invoked lambda
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *p;
  {
    int x = 5;
    p = &x;
  }
  return [&] { return *p; }();
}
