// DESC: a string_view is created from an owner FIELD; a later method call
// mutates (reallocates) that field, invalidating the view. Unlike a local
// owner (whose mutation IS caught), mutating a member owner does not invalidate
// borrows into it, so this is missed.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct S {
  std::string field = std::string(50, 'a'); // heap-allocated (beyond SSO)
  void grow() { field.append(10000, 'b'); }  // reallocates the field

  int f() {
    std::string_view sv = field; // sv borrows field's heap buffer
    grow();                      // reallocates field, invalidating sv
    return sv.size() ? sv[0] : 0;
  }
};

int main() {
  S s;
  return s.f();
}
