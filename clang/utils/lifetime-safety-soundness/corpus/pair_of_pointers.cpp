// DESC: a std::pair carries a pointer to a local out of the function
// EXPECT-ASAN: stack-use-after-return
#include <utility>
__attribute__((noinline)) std::pair<int *, int> f() {
  int x = 7;
  return {&x, 0};
}
int main() {
  auto pr = f();
  return *pr.first;
}
