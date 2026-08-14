// DESC: a standard library specialization calling a user HOOK at static destruction.
// Following a std specialization's template arguments covers what it DESTROYS -- its
// elements -- so `vector<Logger>` was already rejected. But a container also CALLS its
// allocator (and a unique_ptr its deleter, a map its comparator, a basic_string its
// traits) while it is being destroyed at shutdown. Such an argument is normally trivially
// destructible, so the element recursion accepted it and arbitrary user code ran with
// nothing verifying it.
//
// Note both annotations here are TRUTHFUL and verified: `allocate` really does return
// storage that outlives every caller, and `deallocate` really does not let `p` escape. The
// defect is not a lying annotation -- it is that `deallocate`'s body was never a subject of
// the destruction-order check at all.
//
// The names the library invokes on a hook are fixed by the standard, so they can be
// enumerated exhaustively rather than guessed. Identification deliberately uses only the
// names no ordinary type carries: an ELEMENT type has just its destructor called, so a type
// with a `find` or `length` member must not be dragged in by having one.
// EXPECT-ASAN: heap-use-after-free
#include <cstddef>
#include <string>
#include <vector>

volatile char sink;

extern const std::string g;

template <class T> struct MyAlloc {
  using value_type = T;
  MyAlloc() = default;
  template <class U> MyAlloc(const MyAlloc<U> &) {}

  // Truthful: the storage really does outlive every caller.
  [[clang::lifetime_immortal]] T *allocate(std::size_t) {
    static T buf[64];
    return buf;
  }
  // Truthful: `p` really does not escape. It is still user code running at shutdown.
  void deallocate(T *p [[clang::noescape]], std::size_t) {
    sink = g[0]; // g's heap buffer was freed by ~basic_string already
  }
  bool operator==(const MyAlloc &) const { return true; }
};

std::vector<int, MyAlloc<int>> gv(4); // constructed FIRST -> destroyed LAST
const std::string g(70, 'x');         // destroyed FIRST

int main() { sink = (char)gv[0]; }
