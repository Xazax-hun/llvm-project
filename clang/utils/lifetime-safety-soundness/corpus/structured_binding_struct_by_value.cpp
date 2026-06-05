// DESC: address of a by-value struct structured-binding member (the copy dies)
// EXPECT-ASAN: stack-use-after-scope
struct P {
  int x, y;
};
int main() {
  int *q;
  {
    P s{1, 2};
    auto [a, b] = s; // 'a' aliases the local copy's member
    q = &a;
  }
  return *q;
}
