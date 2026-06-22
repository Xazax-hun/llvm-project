// DESC: a const member function mutates an owner through a FREE function that
// launders mutable access -- `launder(content)` takes the unique_ptr by
// `const&` marked [[clang::lifetimebound]] and returns a non-const `std::string*`
// (via `.get()`). The annotation is truthful (the result borrows `content`), so
// it is accepted; but the const method then mutates *content through that
// non-const result, reallocating it while a caller's lifetimebound view borrows
// it. The const-subversion walk followed only member-call object arguments back
// to `this`; a free-call result was not followed through its lifetimebound
// argument. Found by the multi-agent bypass hunt. Fixed by following any
// [[clang::lifetimebound]] argument of a call whose result is a non-const
// indirection back toward `this`.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

std::string *launder(const std::unique_ptr<std::string> &a
                     [[clang::lifetimebound]]) {
  return a.get(); // truthful lifetimebound, but hands out a non-const string*
}

struct Doc {
  std::unique_ptr<std::string> content = std::make_unique<std::string>(60, 'a');
  std::string_view view() const [[clang::lifetimebound]] {
    return std::string_view(*content);
  }
  void grow() const { launder(content)->append(20000, 'b'); } // reallocates *content
};

int main() {
  Doc d;
  std::string_view v = d.view(); // borrows *content (tracked, lifetimebound)
  d.grow();                      // const method reallocates -> v dangles
  return v.size() ? v[0] : 0;    // use-after-free
}
