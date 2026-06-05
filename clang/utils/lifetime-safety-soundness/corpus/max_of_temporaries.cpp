// DESC: std::max returns a reference to a destroyed temporary
// EXPECT-ASAN: stack-use-after-scope
#include <algorithm>
int main() {
  const int &m = std::max(3, 4); // binds to a temporary that is then destroyed
  return m;
}
