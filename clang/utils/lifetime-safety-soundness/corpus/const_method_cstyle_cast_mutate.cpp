// DESC: a const member function mutates a directly-owned [[gsl::Owner]] field
// through an explicit C-style cast that drops `const` (`((std::string&)s).append`
// or `((Box*)this)->s.append`). The model trusts a const method not to invalidate
// borrows into the object; a lifetimebound accessor (`view()`) ties the borrow
// into `s` to the object, so lost-loan/unannotated-indirection don't fire, and
// the const-subversion handler missed it because constDroppedReachingThis walked
// via IgnoreParenImpCasts (stripping implicit casts but not the explicit C-style
// cast) -- so it never saw a const-dropping crossing. Found by the multi-agent
// bypass hunt. Fixed by recognizing an explicit const-dropping cast (and keying
// the arrow crossing on a non-const pointee) in constDroppedReachingThis.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct Box {
  std::string s = std::string(100, 'x');
  std::string_view view() const [[clang::lifetimebound]] { return s; }
  void grow() const { ((std::string &)s).append(200, 'y'); } // const dropped via C-cast
};

int main() {
  Box b;
  std::string_view v = b.view(); // borrow into b.s, tracked (lifetimebound)
  b.grow();                      // const method reallocates s -> v dangles
  return v.size() ? (int)v[0] : 0; // use-after-free
}
