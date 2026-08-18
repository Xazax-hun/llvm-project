// DESC: a lifetimebound accessor called on a temporary object
// EXPECT-ASAN: stack-use-after-scope
#include <string>
struct H {
  std::string s;
  std::string &get() [[clang::lifetimebound]] { return s; }
};
int main() {
  // H{...} is a temporary; r refers to its member after it is destroyed.
  std::string &r = H{"a string long enough to heap allocate xxxxxxxx"}.get();
  return r[0];
}
