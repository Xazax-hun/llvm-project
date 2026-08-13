// DESC: a '[[clang::lifetime_non_invalidating]]' method that reallocates what it
// promises not to touch. The attribute suppresses the assumed-invalidation fact at
// every CALL SITE -- that is its entire purpose, letting a user-defined owner declare
// read accessors the name-based std allow-list cannot recognize. But nothing verified
// the promise against the body, so annotating a method that DOES reallocate silently
// disarmed the check in every caller: the borrow taken before the call was never
// reported as invalidated by it. The attribute was the newest in the model and the
// only one with no body verifier -- lifetimebound, noescape and lifetime_immortal all
// check theirs. Neither -Wlifetime-safety-all nor inference/TU mode said anything.
//
// The fix verifies the promise for the function's INPUTS (the implicit object and the
// parameters), including invalidations the analysis merely ASSUMES: exempting those
// would reopen the hole, since suppressing the assumed channel is exactly what the
// attribute does. Invalidating a local stays legal -- a local dies with the call, so
// no caller borrow can point into it.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

struct Pool {
  std::vector<int> v{1, 2, 3};

  // Untrue: this reallocates the vector the caller is borrowing from.
  [[clang::lifetime_non_invalidating]] void grow() {
    for (int i = 0; i < 64; i++)
      v.push_back(i);
  }
};

int main() {
  Pool p;
  int &a = p.v[0]; // borrow into p.v's buffer
  p.grow();        // reallocates it; the promise hid this
  sink = a;        // heap-use-after-free
  return 0;
}
