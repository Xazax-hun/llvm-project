// DESC: a pointer from std::array::data() outliving the array
// EXPECT-ASAN: stack-use-after-scope
#include <array>
int main() {
  int *p;
  {
    std::array<int, 4> a{1, 2, 3, 4};
    p = a.data(); // p borrows a
  }
  return *p;
}
