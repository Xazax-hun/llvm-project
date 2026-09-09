// DESC: a const method mutating an owner through a pointer member, wrapped in an
// immediately-invoked lambda that captures `this`. The closure is the call's
// receiver, so only IT was invalidated; and the lambda body is analyzed as its own
// function, where the captured `this` is not the object either. The mutation was
// therefore attributed to nothing, and wrapping the statement in a lambda silenced
// the report the identical unwrapped statement produced.
//
// Calling a lambda that captured `this` is now assumed to mutate the enclosing
// object, like every other call whose effect on an owner cannot be seen.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int g_sink;

struct [[gsl::Pointer]] Warmer {
  std::vector<int> *pv;
  // `const`, so callers trust it not to invalidate borrows into what it reaches.
  void warm() const {
    [this] { pv->push_back(1); }();
  }
};

int main() {
  std::vector<int> v{1, 2, 3};
  Warmer w{&v};
  const int &r = v[0]; // borrow into the vector's buffer
  for (int i = 0; i < 200; ++i)
    w.warm(); // a const method reallocates it
  g_sink = r;
  return 0;
}
