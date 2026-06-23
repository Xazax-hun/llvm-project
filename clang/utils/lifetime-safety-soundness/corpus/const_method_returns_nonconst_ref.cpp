// DESC: a const member function hands out a non-const REFERENCE into an owner
// reached from the object (`std::string& ref() const { return *content; }`),
// and a caller mutates the owner through it, dangling a sibling lifetimebound
// view. `const` does not protect the pointee of the returned reference, so the
// const method effectively exports mutable access to its own state. The
// const-subversion check catches this at the ESCAPE site -- the accessor body
// that returns the non-const reference -- which is the root cause, rather than
// at a later mutation through the handed-out reference (which may live in
// another TU). A `const std::string&` return would be fine; a non-const one is
// the crossing.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

struct Doc {
  std::unique_ptr<std::string> content = std::make_unique<std::string>(60, 'a');
  // Non-const reference into the owner, handed out from a const method.
  std::string &ref() const { return *content; }
  std::string_view view() const [[clang::lifetimebound]] {
    return std::string_view(*content);
  }
};

int main() {
  Doc d;
  std::string_view v = d.view(); // borrows *content (tracked, lifetimebound)
  d.ref().append(20000, 'b');    // mutate through the const-handed-out ref
  return v.size() ? v[0] : 0;    // use-after-free
}
