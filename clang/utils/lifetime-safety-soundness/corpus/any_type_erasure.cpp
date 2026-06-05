// DESC: a borrow stored into a type-erased std::any
// EXPECT-ASAN: stack-use-after-scope
#include <any>
int main() {
  std::any a;
  {
    int x = 5;
    a = &x; // type-erased: 'a' holds &x
  }
  return *std::any_cast<int *>(a);
}
