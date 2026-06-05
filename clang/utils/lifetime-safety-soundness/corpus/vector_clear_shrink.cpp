// DESC: a reference into a vector used after the buffer is freed
// EXPECT-ASAN: heap-use-after-free
#include <vector>
int main() {
  std::vector<int> v{1, 2, 3, 4, 5};
  int &r = v[2];
  v.clear();
  v.shrink_to_fit(); // frees the buffer
  return r;
}
