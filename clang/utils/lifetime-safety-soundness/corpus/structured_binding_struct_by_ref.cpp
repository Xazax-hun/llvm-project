// DESC: borrow through a by-ref struct structured-binding member outlives scope
// EXPECT-ASAN: stack-use-after-scope
struct P {
  int x, y;
};
int main() {
  int *q;
  {
    P s{1, 2};
    auto &[a, b] = s; // 'a' aliases s.x
    q = &a;
  }
  return *q;
}
