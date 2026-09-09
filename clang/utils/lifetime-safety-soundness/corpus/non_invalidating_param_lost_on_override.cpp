// DESC: a virtual override silently dropping a parameter-level
// [[clang::lifetime_non_invalidating]]. The promise is consumed at the CALL SITE
// against the statically resolved callee, so a call through the base suppresses the
// assumed invalidation -- while dispatch runs the override, whose parameter carries
// no promise and whose body is therefore never verified. Truthful base, lying
// override, nothing reported at either end.
//
// The method-level form of the attribute already required an override to repeat the
// promise; the parameter-level form did not.
// FLAGS: -Wno-lifetime-safety-unannotated-indirection
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int g_sink;

struct Base {
  virtual ~Base() = default;
  virtual void touch(std::vector<int> &v [[clang::noescape]]
                                         [[clang::lifetime_non_invalidating]]) const {}
};

struct Derived : Base {
  // Drops the promise and reallocates.
  void touch(std::vector<int> &v [[clang::noescape]]) const override {
    v.push_back(42);
  }
};

static void run(Base &b [[clang::noescape]]) {
  std::vector<int> v{1, 2, 3};
  int *p = &v[0];
  b.touch(v); // statically Base::touch, dynamically Derived::touch
  g_sink = *p;
}

int main() {
  Derived d;
  run(d);
  return 0;
}
