// DESC: a scope guard held as a lambda INIT-CAPTURE. Destroying a closure runs
// every capture's destructor, so `[g = Grower{&vec}]{}` reallocates the borrowed
// vector when the closure dies. The mutation test asked whether the destroyed type
// is a pointer/reference or a gsl::Pointer wrapper -- a closure is neither, so it
// said no. A closure is also exempt from the unknown-ownership ban (a lambda value
// is modeled directly) and carries no annotation, so nothing else covered it, while
// the same guard held by an annotated wrapper (caught) or by a plain struct
// (refused as unknown-ownership) was reported. Asking the mutation question of each
// capture is what keeps a by-value capture of an OWNER silent: that capture is a
// copy, and destroying a copy invalidates no borrow of the original.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile char g_sink = 0;

struct [[gsl::Pointer]] Grower {
  std::vector<int> *v;
  ~Grower() { v->resize(4000); } // reallocates the borrowed vector
};

int main() {
  std::vector<int> vec(1, 5);
  int *p = &vec[0];
  {
    auto f = [g = Grower{&vec}] {};
    (void)f;
  }                  // ~Grower runs when the closure dies
  g_sink = (char)*p; // heap-use-after-free
  return 0;
}
