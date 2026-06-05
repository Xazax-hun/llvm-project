// DESC: borrow stored into a multi-dimensional array element (shared origin)
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *a[2][2];
  {
    int x = 11;
    a[0][1] = &x;
  }
  return *a[0][1];
}
