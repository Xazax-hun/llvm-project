// DESC: a pointer into a vector element dangles after a reallocation
// EXPECT-ASAN: heap-use-after-free
#include <vector>
int main() {
  std::vector<int> v{1, 2, 3};
  int *p = &v[0];
  for (int i = 0; i < 1000; ++i)
    v.push_back(i); // reallocates, invalidating p
  return *p;
}
