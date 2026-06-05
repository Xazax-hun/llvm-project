// DESC: a structured binding aliases a pointer to a local that then dies
// EXPECT-ASAN: stack-use-after-scope
struct Pair {
  int *a;
  int *b;
};
int main() {
  int *q;
  {
    int x = 5;
    Pair pr{&x, &x};
    auto [a, b] = pr;
    q = a; // q = &x
  }
  return *q;
}
