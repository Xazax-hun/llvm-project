// DESC: two arguments of one call that alias, where the mutating one is upcast to
// an abstract interface. The overlap check asked whether the mutable-reference
// parameter could reach an owner from its STATIC type, and an interface declares
// no data members -- so nothing looked mutable, while the virtual call inside
// dispatches into the derived object and frees the std::string being viewed.
// Spelling the parameter with the derived type reported it, so one upcast
// silenced the same bug. Sibling of virtual_param_reentrancy_base_upcast.cpp,
// which fixed this erasure in the assumed-invalidation RECEIVER gate; here the
// borrow arrives as a separate argument, so the hazard is the aliasing of two
// arguments and it went through the overlap gate instead. The callee is beyond
// reproach: both [[clang::noescape]] annotations are truthful and nothing tells it
// its two parameters may be one object. "Is the pointee polymorphic" is
// deliberately not the gate -- see nonpoly_base_upcast_reentrancy.cpp.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink;

struct IResettable {
  virtual ~IResettable() = default;
  virtual void reset() = 0;
};

struct [[gsl::Owner]] Record : IResettable {
  std::string name = "0123456789abcdefghijklmnopqrstuvwxyz0123456789ABCDEF";
  std::string_view getName() const [[clang::lifetimebound]] { return name; }
  void reset() override {
    name.clear();
    name.shrink_to_fit(); // actually releases the buffer
  }
};

void resetAndLog(IResettable &target [[clang::noescape]],
                 std::string_view label [[clang::noescape]]) {
  target.reset();
  g_sink = label[0]; // heap-use-after-free
}

int main() {
  Record r;
  resetAndLog(r, r.getName()); // the two arguments are the same object
  return 0;
}
