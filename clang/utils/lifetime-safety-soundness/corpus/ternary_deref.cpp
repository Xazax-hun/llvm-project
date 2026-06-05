// DESC: dereference of a dangling pointer selected by a conditional operator
// EXPECT-ASAN: stack-use-after-scope
__attribute__((noinline)) int pick();
int main() {
  int *p;
  {
    int x = 3;
    p = &x;
  }
  int y = 9;
  return *(pick() ? p : &y); // p (dangling) chosen when pick() is true
}
int pick() { return 1; }
