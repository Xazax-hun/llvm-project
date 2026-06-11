// DESC: a user-defined [[gsl::Owner]] with a non-const, [[clang::lifetimebound]]
// method that mutates (reallocates) its buffer. A view from a sibling accessor
// is held across it. The method is not a recognized std non-invalidating
// accessor, so a non-const call on the owner must be assumed to invalidate --
// returning a borrow of *this is orthogonal to mutating.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct [[gsl::Owner(char)]] MyStr {
  std::string buf = std::string(60, 'a');
  // Fluent mutator that also hands out a borrow of *this (lifetimebound).
  MyStr &append(const char *s [[clang::noescape]]) [[clang::lifetimebound]] {
    buf += s;
    return *this;
  }
  std::string_view view() const [[clang::lifetimebound]] {
    return std::string_view(buf);
  }
};

int main() {
  MyStr s;
  std::string_view v = s.view();         // v borrows s.buf
  s.append("more than enough characters to force a heap reallocation here!!");
  return v.size() ? v[0] : 0;            // use-after-invalidation
}
