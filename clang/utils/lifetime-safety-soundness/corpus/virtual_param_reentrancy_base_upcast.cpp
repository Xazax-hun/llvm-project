// DESC: reentrancy through a base-typed reference parameter. Assumed-invalidation
// decided whether a mutated parameter could reach an owner from the parameter's
// STATIC type, so upcasting the argument to an abstract interface erased the edge:
// `Reloader` has no data members, so nothing looked mutable -- while the virtual
// call in notifyAll dispatches straight back to the derived App and reallocates the
// std::string being viewed. The [[clang::noescape]] is truthful (R really does not
// escape), so no body verifier applies, and no annotation expresses "this virtual
// call may invalidate anything reachable from the argument's complete object".
// Declaring the parameter `App&` instead of `Reloader&` warned; the upcast did not.
// A virtual interface is also the only callback mechanism the safe model permits
// (std::function -> multilevel-indirection, function pointers -> indirect-call,
// capturing lambdas in plain structs -> unknown-ownership), so this was the one
// callback shape with a hole.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Reloader {
  virtual ~Reloader() = default;
  virtual void reload() = 0;
};

// Truthful annotation: R genuinely does not escape this function.
static void notifyAll(Reloader &R [[clang::noescape]]) { R.reload(); }

struct App : Reloader {
  std::string Cfg = "a configuration blob string long enough to be heap alloc";

  void reload() override { Cfg = std::string(400, 'z'); } // reallocates Cfg

  void go() {
    std::string_view V = Cfg; // borrow into this->Cfg
    notifyAll(*this);         // virtual dispatch comes back and reallocates it
    sink = V[0];              // heap-use-after-free
  }
};

int main() {
  App A;
  A.go();
  return 0;
}
