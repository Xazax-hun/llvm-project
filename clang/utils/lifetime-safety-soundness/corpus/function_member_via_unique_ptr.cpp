// DESC: a std::function member holding a by-reference-capturing closure is
// reached through a unique_ptr (`h->fn = [&local]{...}`); the borrowed local
// dies while the heap-allocated Holder (and its closure) lives on, then the
// closure is invoked. The captured borrow is hidden inside the type-erased
// std::function, so the element type Holder looks borrow-free at the type level
// -- but a std::function can capture a borrow, so an unannotated Holder holding
// one is an owner-of-indirection just like a raw view member: unique_ptr<Holder>
// is untracked until Holder is annotated. Same family as
// view_field_via_unique_ptr.cpp / vector_of_function_capture.cpp, laundered
// through a trusted owner's operator->.
// EXPECT-ASAN: heap-use-after-free
#include <functional>
#include <memory>
#include <string>

struct Holder {
  std::function<std::string()> fn;
};

int main() {
  auto h = std::make_unique<Holder>();
  {
    std::string local = "LOCAL backing string long enough to live on the heap!!";
    h->fn = [&local] { return local; }; // closure captures local by reference
  }                                      // local dies; h (and h->fn) live on
  return static_cast<int>(h->fn().size()); // invoke dangling closure
}
