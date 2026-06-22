// DESC: a const member function reallocates an owner reached through an owning
// smart pointer (`std::unique_ptr<std::string>`) that is a NESTED member
// (`this->a.sp`, not a direct `this->member`). `const` does not propagate through
// the unique_ptr, so the pointee string is mutable; a caller's lifetimebound view
// into it dangles when the const method reserves/reallocates. The const-
// subversion check recognized an owning-smart-pointer member only as a direct
// `this->member` (isThisExpr on the receiver base), so the one-sub-object-deeper
// `this->a.sp->reserve()` slipped. Found by the multi-agent bypass hunt. Fixed by
// following a chain of const-propagating value-subobject accesses from `this`
// (thisRootedValueMember).
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

struct Inner {
  std::unique_ptr<std::string> sp =
      std::make_unique<std::string>("this is a long heap string xxxxxxxxxxxxx");
};

struct Box {
  Inner a;
  std::string_view view() const [[clang::lifetimebound]] { return *a.sp; }
  void grow() const { a.sp->reserve(8192); } // const, but reallocates *a.sp
};

int main() {
  Box b;
  std::string_view v = b.view(); // tracked borrow into *a.sp (lifetimebound)
  b.grow();                      // const method reallocates -> v dangles
  return v.size() ? (int)v[0] : 0; // use-after-free
}
