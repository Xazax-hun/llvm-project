// DESC: a struct with a C-ARRAY member of an owner-of-indirection container
// (`std::vector<std::string_view> arr[2]`) was not flagged: the owner-/pointer-
// of-indirection member walk (and findNestedOwnerOrPointerOfIndirection) did not
// peel array dimensions, so the array-of-vector-of-views member slipped while
// the scalar `std::vector<std::string_view> v` form was rejected. make() pushed
// a string_view into a local string onto arr[0] and returned the struct,
// leaving a dangling view, silently. Array dimensions are now peeled.
// EXPECT-ASAN: heap-use-after-free
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
struct S { std::vector<std::string_view> arr[2]; };
__attribute__((noinline)) S make() {
  S s;
  std::string local = "a long heap string exceeding sso limits..........";
  s.arr[0].push_back(local); // string_view into local
  return s;                  // local dies; returned view dangles
}
int main() {
  S s = make();
  std::string_view sv = s.arr[0][0];
  printf("%zu %c\n", sv.size(), sv.empty() ? '?' : sv[0]); // reads freed heap
  return 0;
}
