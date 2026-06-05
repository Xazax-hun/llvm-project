// DESC: a std::optional<int*> carries a pointer to a local out of the function
// EXPECT-ASAN: stack-use-after-return
#include <optional>
__attribute__((noinline)) std::optional<int *> f() {
  int x = 5;
  return &x;
}
int main() {
  auto o = f();
  return **o;
}
