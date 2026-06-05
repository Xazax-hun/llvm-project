// DESC: a borrow assigned to a pointer through std::tie
// EXPECT-ASAN: stack-use-after-scope
#include <tuple>
int main() {
  int *p = nullptr;
  {
    int x = 7;
    std::tie(p) = std::make_tuple(&x);
  }
  return *p;
}
