// DESC: a non-const base method that reaches a derived owner by downcasting
// (static_cast<Derived*>(this)) is called through a BASE REFERENCE bound to the
// derived object (Base& b = d; b.grow()). The derived-to-base conversion is at
// the binding, not the call, so the call-site receiver type is the owner-less
// Base, and the method is non-virtual (so the polymorphic-receiver rule does not
// fire) -- the assumed-invalidation was skipped and a string_view from a sibling
// lifetimebound accessor dangled after the realloc. The receiver type is now
// recovered by following the reference binding to the bound object.
// EXPECT-ASAN: heap-use-after-free
#include <cstdio>
#include <string>
#include <string_view>
struct Base { void grow(); };
struct Derived : Base {
  std::string buf{"a long heap string exceeding sso limits.........."};
  std::string_view view() const [[clang::lifetimebound]] { return buf; }
};
inline void Base::grow() { static_cast<Derived *>(this)->buf.push_back('z'); }
__attribute__((noinline)) int bug() {
  Derived d;
  std::string_view sv = d.view(); // borrow into d.buf
  Base &b = d;                    // slice to the owner-less base
  for (int i = 0; i < 200; ++i)
    b.grow();                     // reallocates d.buf via the base ref
  return sv.empty() ? 0 : sv[0];  // heap-use-after-free
}
int main() {
  printf("%d\n", bug());
  return 0;
}
