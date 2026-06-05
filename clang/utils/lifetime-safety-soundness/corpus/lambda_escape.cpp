// DESC: a std::function stores a lambda capturing a local by reference
// EXPECT-ASAN: stack-use-after-scope
#include <functional>
int main() {
  std::function<int()> fn;
  {
    int x = 33;
    fn = [&]() { return x; };
  }
  return fn();
}
