// DESC: a hazard inside a DEFAULT MEMBER INITIALIZER of a container's element type. The
// type-level construction question enumerated a class's constructors, bases and fields --
// and a field's TYPE says nothing about its in-class initializer, which the generated
// constructor runs. So `struct Logger { int n = peek(); };` answered "safe" to every
// type-level rule while constructing it called `peek`.
//
// That is invisible exactly where the type-level answer is all there is: inside a library
// container. `m.emplace_back()` on a `std::vector<Logger>` builds an element from inside
// libc++, so no construction in the verified body describes it and nothing is reported
// precisely -- the element type's own answer has to carry the whole weight.
//
// This shape needs NO annotation anywhere: `struct Entry { int id = next_id(); };` is
// ordinary code, and `__attribute__((destructor))` is shutdown code by declaration rather
// than by promise. The same hole reached through a written constructor is caught, which is
// what localizes this to the initializer rather than to the container rule.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <vector>

volatile char sink;

extern std::string g_name;

int peek();

// No written constructor: the implicit one runs the initializer below.
struct Logger {
  int n = peek();
};

// Runs after the ordinary static destructors, so g_name's buffer is already freed.
__attribute__((destructor(101))) static void at_shutdown() {
  std::vector<Logger> box;
  box.emplace_back();
  sink = (char)box[0].n;
}

std::string g_name = "a-global-string-long-enough-that-its-buffer-is-heap-allocated";

int peek() {
  sink = g_name.data()[0];
  return 0;
}

int main() { return 0; }
