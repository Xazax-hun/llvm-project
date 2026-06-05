// DESC: a string_view bound to a temporary string dangles immediately
// EXPECT-ASAN: use-after-free
#include <string>
#include <string_view>
int main() {
  std::string a = "a string long enough to heap allocate xxxxxxxx";
  std::string b = "another string long enough to heap allocate yyy";
  std::string_view sv = a + b; // views the temporary (a + b)
  return sv.size() ? sv[0] : 0;
}
