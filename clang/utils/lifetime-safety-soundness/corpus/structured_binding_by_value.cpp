// DESC: address of a by-value array structured-binding element (the copy dies)
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *q;
  {
    int arr[2] = {1, 2};
    auto [a, b] = arr; // 'a' aliases the local copy's element
    q = &a;
  }
  return *q;
}
