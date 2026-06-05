// DESC: a lambda init-capture stores a borrow to a local that then dies
// EXPECT-ASAN: stack-use-after-return
#include <functional>
__attribute__((noinline)) std::function<int()> f() {
  int x = 7;
  return [p = &x]() { return *p; }; // init-capture [p = &x]; x dies on return
}
int main() { return f()(); }
