// DESC: a std::function returning a reference to a captured local
// EXPECT-ASAN: stack-use-after-scope
#include <functional>
int main() {
  std::function<int &()> f;
  {
    int x = 3;
    f = [&x]() -> int & { return x; };
  }
  return f();
}
