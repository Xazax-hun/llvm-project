// DESC: reentrancy through a base-typed reference parameter where the base is NOT
// polymorphic. Assumed-invalidation took reachability from the PARAMETER's static
// type, so upcasting the argument (`notify(*this)` with a `Base &` parameter) erased
// the edge to the owner. Gating on "the pointee is polymorphic" was the wrong proxy:
// virtual dispatch is only one way back down to the derived object -- a plain
// `static_cast<App &>(b)` in the callee reaches it with no vtable at all.
// Reachability now comes from the ARGUMENT's static type, which survives the
// conversion to the base and still shows the owner; the parameter only decides
// whether the callee can mutate through it. The [[clang::noescape]] is truthful, so
// no body verifier applies, and no annotation expresses this hazard.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Base {
  int tag = 0;
}; // no virtual functions

void notify(Base &b [[clang::noescape]]);

struct App : Base {
  std::string cfg = "a configuration string long enough to need a heap buffer";
  void go() {
    std::string_view v = cfg;
    notify(*this);  // static type at the parameter is Base&
    sink = v[0];    // heap-use-after-free
  }
};

void notify(Base &b [[clang::noescape]]) {
  static_cast<App &>(b).cfg = std::string(500, 'z'); // reallocates, no vtable
}

int main() {
  App a;
  a.go();
  return 0;
}
