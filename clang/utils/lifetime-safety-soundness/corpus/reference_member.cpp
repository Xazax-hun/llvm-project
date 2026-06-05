// DESC: a struct with a reference member returns a dangling reference
// EXPECT-ASAN: stack-use-after-return
struct Wrap {
  const int &r;
};
__attribute__((noinline)) const int &f() {
  Wrap w{42}; // w.r binds to a temporary
  return w.r;
}
int main() { return f(); }
