// DESC: a NAMED callable handed to a standard algorithm from a verified destructor.
// `std::for_each(first, last, Doit{})` invokes `Doit::operator()` from inside the library,
// and none of that call appears in the body being verified -- so the functor's body ran at
// shutdown entirely unchecked. A lambda written in the same place was traversed and caught
// all along, which is what made the gap look closed.
//
// This is the same trust as for a library call's receiver, applied to what the call is
// HANDED: a library callee is trusted for where it was WRITTEN, which says nothing about
// the types it is given.
//
// Which member the library calls cannot be known from the call site -- an algorithm may
// copy, assign, compare, dereference, invoke or destroy what it is handed, and which of
// those it does is a property of the library's implementation. Enumerating the operators
// it might use was tried first and is both fragile and incomplete: the same hole exists
// through a user CONVERSION operator on the element type, and through a user iterator's
// `operator*`. So the rule is type-based -- the whole type must be verified -- and
// annotating the class is what does that.
//
// The iterators here are deliberately pointer-free (an index iterator over a plain int
// range) so that nothing borrow-holding is passed: with a pointer or a container iterator,
// -Wlifetime-safety-unannotated-indirection fires on the library's own unannotated
// parameters and masks this incidentally.
// EXPECT-ASAN: heap-use-after-free
#include <algorithm>
#include <string>

volatile char sink;

extern std::string g_s;

// A hand-written iterator holding no pointer, so no borrow is passed to the library.
struct IndexIterator {
  int i;
  int operator*() const { return i; }
  IndexIterator &operator++() {
    ++i;
    return *this;
  }
  bool operator!=(IndexIterator o) const { return i != o.i; }
  bool operator==(IndexIterator o) const { return i == o.i; }
  using value_type = int;
  using difference_type = int;
  using iterator_category = std::input_iterator_tag;
  using pointer = const int *;
  using reference = const int &;
};

// Trivially destructible, so no type-level destruction rule constrains it. Its
// operator() is the hazard, and the library is what calls it.
struct Doit {
  void operator()(int) const { sink = g_s.data()[0]; }
};

struct [[clang::destruction_order_safe]] Trigger {
  ~Trigger() { std::for_each(IndexIterator{0}, IndexIterator{1}, Doit{}); }
};

// Dynamically initialized FIRST, so destroyed LAST -- after g_s's buffer is freed.
Trigger g_trigger;

std::string g_s = "a-global-string-long-enough-that-its-buffer-is-heap-allocated";

int main() {
  sink = g_s.data()[0];
  return 0;
}
