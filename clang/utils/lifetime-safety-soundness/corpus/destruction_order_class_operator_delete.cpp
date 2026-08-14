// DESC: a class-specific `operator delete` running at static destruction. A deallocation
// function is user code that runs when an owner destroys the object -- `unique_ptr<T>`'s
// destructor calls it -- but it is not a destructor, so nothing in the destruction-order
// rules reached it.
//
// The type-level rule short-circuited on "trivially destructible, so nothing runs at
// shutdown". That is exactly backwards here: a type with no destructor is precisely the
// case where the shortcut fires while an owner of it still calls this function. `Foo` was
// therefore a "safe" type and `std::unique_ptr<Foo>` a legal static, with arbitrary user
// code running during static destruction and nothing looking at it.
//
// A direct `delete` in a verified body was already blocked incidentally (the raw pointer
// draws -Wlifetime-safety-unannotated-indirection), which is why the live route is through
// a smart pointer: its `delete` sits in a system header the body checker never sees. So the
// check has to be on the TYPE, not on the delete-expression.
//
// Annotating the deallocation function is the escape hatch, and puts its body through the
// same verifier. A user-REPLACED *global* `operator delete` needs no annotation: it runs
// whenever anything is freed and there is no type at fault to ban, so it is verified on
// sight, like '__attribute__((destructor))'.
// EXPECT-ASAN: heap-use-after-free
#include <cstddef>
#include <memory>
#include <string>

volatile char sink;

extern std::string gs;

// Trivially destructible: no destructor runs at shutdown. This does.
struct Foo {
  int v = 1;
  static void operator delete(void *p, std::size_t) noexcept {
    sink = gs[0]; // gs's buffer was freed by ~basic_string already
    ::operator delete(p);
  }
};

std::unique_ptr<Foo> gu = std::make_unique<Foo>(); // dynamic init #1 -> freed LAST
std::string gs =                                   // #2 -> destroyed FIRST
    "hello there long enough to heap allocate xxxxxxxxxxxxxxxxxxxxxxxxx";

int main() { return 0; }
