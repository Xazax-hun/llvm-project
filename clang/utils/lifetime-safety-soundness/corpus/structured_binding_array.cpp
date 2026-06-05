// DESC: address of an array structured-binding element outliving the array
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *q;
  {
    int arr[2] = {1, 2};
    auto &[a, b] = arr;
    q = &a; // q = &arr[0]
  }
  return *q;
}
