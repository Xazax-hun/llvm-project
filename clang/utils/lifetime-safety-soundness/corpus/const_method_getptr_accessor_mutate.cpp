// DESC: a const member function mutates an owner reached through a sibling const
// ACCESSOR that hands out a non-const pointer into the owner (`unique_ptr::get()`
// re-exposed as `std::string* getPtr() const`). `const` does not protect the
// pointee, so the accessor returns mutable access; using it (`getPtr()->append`)
// reallocates the owner while a caller's lifetimebound view borrows it. The
// const-subversion check recognized the const-drop only via `operator*`/`->` on a
// member; a `.get()`/accessor-returned non-const pointer slipped. Found by the
// multi-agent bypass hunt. Fixed by the principle that in a const method any
// `this`-derived expression with an indirection type whose pointee is non-const
// is a crossing -- now caught at the ESCAPE site (the accessor body that returns
// the non-const pointer is the root cause), with the loan provenance flowing
// through the dataflow regardless of how the borrow is spelled.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

struct Doc {
  std::unique_ptr<std::string> content = std::make_unique<std::string>(60, 'a');
  std::string *getPtr() const { return content.get(); } // non-const access from const
  std::string_view view() const [[clang::lifetimebound]] {
    return std::string_view(*content);
  }
  void grow() const { getPtr()->append(20000, 'b'); } // reallocates *content
};

int main() {
  Doc d;
  std::string_view v = d.view(); // borrows *content (tracked, lifetimebound)
  d.grow();                      // const method reallocates -> v dangles
  return v.size() ? v[0] : 0;    // use-after-free
}
