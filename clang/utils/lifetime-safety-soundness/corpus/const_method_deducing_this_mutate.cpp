// DESC: a C++23 deducing-this const member function (`this const Doc& self`)
// mutates an owner through the pointee of a unique_ptr member reached via the
// explicit object parameter. The explicit object reference is `const`, so the
// analysis trusts that the method does not invalidate borrows into the object,
// yet `const` does not propagate through the smart pointer -- the pointee is
// mutable. A sibling lifetimebound accessor hands out a view into the same
// pointee, which the `const` "grow" then reallocates and dangles. The
// const-subversion check is `const`-trusted-object aware for both the implicit
// `const this` and the deducing-this explicit object parameter; the loan
// provenance flows through `self` identically.
// FLAGS: -std=c++2b
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

struct Doc {
  std::unique_ptr<std::string> content = std::make_unique<std::string>(60, 'a');
  std::string_view view() const [[clang::lifetimebound]] {
    return std::string_view(*content);
  }
  // const (explicit object is a `const` reference), yet mutates *content.
  void grow(this const Doc &self) { self.content->append(20000, 'b'); }
};

int main() {
  Doc d;
  std::string_view v = d.view(); // borrows *content (tracked, lifetimebound)
  d.grow();                      // const method reallocates -> v dangles
  return v.size() ? v[0] : 0;    // use-after-free
}
