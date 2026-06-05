// DESC: an iterator used after the element it refers to is erased
// EXPECT-ASAN: heap-use-after-free
#include <map>
int main() {
  std::map<int, int> m{{1, 10}, {2, 20}};
  auto it = m.find(1);
  m.erase(1); // invalidates 'it'
  return it->second;
}
