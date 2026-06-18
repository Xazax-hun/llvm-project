// DESC: a plain (non-template) aggregate `struct Outer { Inner inner; }` whose
// member record `Inner { std::vector<std::string_view> v; }` transitively holds
// an owner-of-indirection was classified a clean value:
// findNestedOwnerOrPointerOfIndirection only descended template arguments, not
// plain record member fields. The only backstop was the field-level flag at
// Inner's own definition -- suppressed when Inner lives in a system header -- so
// a local/return of Outer capturing a string_view into a scoped std::string
// dangled silently. findNestedOwnerOrPointerOfIndirection now descends plain
// record members too, so Outer (local/return/member) is flagged regardless.
// EXPECT-ASAN: heap-use-after-free
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
struct Inner { std::vector<std::string_view> v; };
struct Outer { Inner inner; };
__attribute__((noinline)) Outer make() {
  Outer o;
  {
    std::string s = "a long heap string exceeding sso limits..........";
    o.inner.v.push_back(s); // string_view(s); s dies at block end
  }
  return o; // returned Outer holds a dangling view
}
int main() {
  Outer o = make();
  printf("%c\n", o.inner.v[0].data()[0]); // reads freed heap
  return 0;
}
