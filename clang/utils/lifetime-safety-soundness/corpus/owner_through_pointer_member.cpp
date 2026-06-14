// DESC: a borrow is taken DIRECTLY from a std::vector owner, while a separate
// [[gsl::Pointer]] wrapper holds a raw pointer to that same owner and reallocates
// it through a non-const method. The wrapper and the direct borrow alias the same
// storage; assumed-invalidation now invalidates the wrapper's *pointee* loans (a
// gsl::Pointer carries its borrows on the pointee origin), reaching the directly
// taken borrow even though it does not flow through the wrapper object.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct [[gsl::Pointer]] Wrap {
  std::vector<int> *v;
  Wrap(std::vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
  void grow() {
    for (int i = 0; i < 1000; ++i)
      v->push_back(i); // reallocates *v
  }
};

volatile int g;

int main() {
  std::vector<int> data;
  data.push_back(42);
  Wrap w(&data);
  int &r = data[0]; // borrow taken directly from data (aliases w.v)
  w.grow();         // reallocates data through w -> r dangles
  g = r;            // heap-use-after-free
  return (int)g;
}
