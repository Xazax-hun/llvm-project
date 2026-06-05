// DESC: a borrow stored into a std::variant alternative
// EXPECT-ASAN: stack-use-after-scope
#include <variant>
int main() {
  std::variant<int *, double> v;
  {
    int x = 5;
    v = &x;
  }
  return *std::get<int *>(v);
}
