// DESC: a std::reference_wrapper rebound to a local that then goes out of scope
// EXPECT-ASAN: stack-use-after-scope
#include <functional>
int main() {
  int y = 0;
  std::reference_wrapper<int> r(y);
  {
    int x = 5;
    r = std::ref(x); // r now refers to the soon-dead 'x'
  }
  return r.get(); // reads the destroyed 'x'
}
