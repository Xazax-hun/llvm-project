// DESC: returning c_str() of a local std::string
// EXPECT-ASAN: heap-use-after-free
#include <string>
__attribute__((noinline)) const char *f() {
  std::string s = "a string long enough to heap allocate xxxxxxxx";
  return s.c_str(); // dangles when s is destroyed
}
int main() { return f()[0]; }
