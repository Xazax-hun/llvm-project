// DESC: a const member function mutates an owner (std::string) through the
// pointee of a std::unique_ptr member. `const` does not propagate through the
// smart pointer, so the pointee is mutable; the analysis would otherwise trust
// that a const member function does not invalidate borrows into the object. A
// sibling lifetimebound accessor hands out a view into the same pointee, which
// the const "grow" then dangles.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

struct Doc {
  std::unique_ptr<std::string> content = std::make_unique<std::string>(60, 'a');
  std::string_view view() const [[clang::lifetimebound]] {
    return std::string_view(*content);
  }
  // const, yet mutates *content through the smart pointer -> reallocation.
  void grow() const { content->append(20000, 'b'); }
};

int main() {
  Doc d;
  std::string_view v = d.view(); // borrows *content
  d.grow();                      // reallocates *content -> v dangles
  return v.size() ? v[0] : 0;    // use-after-invalidation
}
