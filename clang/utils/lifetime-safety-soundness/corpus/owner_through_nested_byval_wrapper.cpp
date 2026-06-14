// DESC: a NESTED by-value [[gsl::Pointer]] wrapper -- a gsl::Pointer (W) whose
// member is itself a by-value gsl::Pointer (Inner), each with a
// lifetime_capture_by(this) constructor. The borrowed owner is reached TWO
// pointee levels below the receiver (`w.in.grow()`: W -> Inner -> vector). A
// gsl::Pointer record is a leaf in the origin tree, so a borrow captured into
// the whole object `w` lives on `w`'s own origin, unreachable from the freshly
// built `w.in` receiver origin. Assumed-invalidation now walks the origin-tree
// parent chain of the receiver, so the enclosing object `w` (which holds the
// borrow) is invalidated too.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct [[gsl::Pointer]] Inner {
  std::vector<int> *v;
  Inner(std::vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
  void grow() {
    for (int i = 0; i < 1000; ++i)
      v->push_back(i); // reallocates the aliased owner
  }
};

struct [[gsl::Pointer]] W {
  Inner in;
  W(Inner i [[clang::lifetime_capture_by(this)]]) : in(i) {}
};

volatile int g_sink;

int main() {
  std::vector<int> data;
  data.push_back(1);
  W w{Inner{&data}};
  int &r = data[0]; // borrow taken directly from the owner (aliases w.in.v)
  w.in.grow();      // reallocates data two pointee levels down -> r dangles
  g_sink = r;       // heap-use-after-free
  return g_sink;
}
