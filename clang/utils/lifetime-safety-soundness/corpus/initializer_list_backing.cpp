// DESC: a pointer into the backing array of a std::initializer_list
// EXPECT-ASAN: stack-use-after-return
#include <initializer_list>
__attribute__((noinline)) const int *f() {
  std::initializer_list<int> il = {10, 20, 30}; // backing array is a temporary
  return il.begin();
}
int main() { return *f(); }
