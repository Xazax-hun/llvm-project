// DESC: borrow stored into an array element, used after the borrowed local dies
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *arr[4];
  {
    int x = 7;
    arr[0] = &x;
  }
  return *arr[0];
}
