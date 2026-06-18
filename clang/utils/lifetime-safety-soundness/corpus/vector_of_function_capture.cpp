// DESC: a std::vector<std::function<...>> built from a braced init-list / factory
// return drops a borrow captured by a stored closure. isStdCallableWrapperType
// (std::function) and lambdas returned false from isUnknownOwnershipType, so a
// std::function/lambda element was not counted as an indirection -- unlike
// std::string_view -- so std::vector<std::function<...>> was not a
// container-of-indirection, and a braced-init/factory-return construction (no
// callable parameter) also dodged unannotated-indirection. A closure capturing a
// local std::string by reference, stored in such a vector and returned, dangled
// silently. A callable-wrapper/lambda element now counts as an indirection.
// EXPECT-ASAN: stack-use-after-return
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
__attribute__((noinline)) std::vector<std::function<char()>> make() {
  std::string s = "hello world long enough to heap allocate this string!!";
  return std::vector<std::function<char()>>{[&s]() { return s[0]; }};
}
int main() {
  auto v = make();   // closures capture make()'s local `s` by reference
  printf("%c\n", v[0]()); // reads the dead `s`
  return 0;
}
