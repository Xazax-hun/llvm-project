// DESC: a free function mutates a mutable owner through a pointer member of its
// const-reference parameter (shallow const: `const` does not protect a pointee
// reached through a unique_ptr). The const-subversion check used to fire only
// inside a const *member* function (for `this`); a const-reference/pointer
// parameter is an equally-trusted indirection to a const value, so mutating its
// pointee behind the caller's back is now flagged too. The caller f() holds a
// borrow into the same owner and trusts the const& parameter.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <memory>

volatile char sink;

struct [[gsl::Owner(char)]] PImpl {
  std::unique_ptr<std::string> impl;
  std::string_view view() const [[clang::lifetimebound]] { return *impl; }
};

void grow(const PImpl& x [[clang::noescape]]) {
  x.impl->reserve(100000); // mutates *x.impl behind a const PImpl& parameter
}
void f(const PImpl& x [[clang::noescape]]) {
  std::string_view v = x.view(); // borrow into *x.impl
  grow(x);                       // reallocates *x.impl behind the const& parameter
  sink = v[0];                   // use-after-free
}

int main() {
  PImpl x;
  x.impl.reset(new std::string(64, 'x'));
  f(x);
  return 0;
}
