// DESC: a borrow laundered through std::bit_cast to an integer and back
// EXPECT-ASAN: stack-use-after-scope
#include <bit>
#include <cstdint>
int main() {
  int *p;
  {
    int x = 9;
    p = std::bit_cast<int *>(std::bit_cast<uintptr_t>(&x));
  }
  return *p;
}
