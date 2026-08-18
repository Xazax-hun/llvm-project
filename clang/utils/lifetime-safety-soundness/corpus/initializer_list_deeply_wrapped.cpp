// DESC: an initializer_list buried nine wrappers deep. `std::initializer_list` does not own
// its elements: declaring one at static storage duration makes the compiler synthesize a
// SEPARATE backing array, also of static storage duration, and it is that array's elements
// which are destroyed at shutdown -- by __cxx_global_array_dtor, running an arbitrary
// element destructor with nothing having verified it.
//
// Neither the list's own type nor an aggregate holding one describes that object, and both
// are trivially destructible, so the walk that finds it has to descend PAST a trivially
// destructible type. That is exactly why it could not carry a depth limit: unlike every
// sibling predicate, which stops at such a type, this one keeps going. A limit answering
// "safe" at the boundary reopened the hole at nine wrappers -- eight was diagnosed, nine
// was silent -- and answering "unsafe" instead reported every deeply nested aggregate,
// including ones holding no initializer_list at all.
//
// A by-value member graph is acyclic, since a class cannot contain itself, so the recursion
// terminates without a limit; a visited set keeps it linear on a wide one.
// EXPECT-ASAN: heap-use-after-free
#include <initializer_list>
#include <string>

volatile char sink;

struct Logger {
  ~Logger();
};

// Nine levels of trivially destructible wrappers.
struct W1 {
  std::initializer_list<Logger> il;
};
struct W2 { W1 m; };
struct W3 { W2 m; };
struct W4 { W3 m; };
struct W5 { W4 m; };
struct W6 { W5 m; };
struct W7 { W6 m; };
struct W8 { W7 m; };
struct W9 { W8 m; };

// The backing array is dynamically initialized here, first, so it is destroyed LAST. Nine
// braces for the wrappers, then one more for the list itself.
W9 g_w = {{{{{{{{{{Logger(), Logger()}}}}}}}}}};

std::string g_name = "a-global-string-long-enough-that-its-buffer-is-heap-allocated";

// Runs from __cxx_global_array_dtor after g_name's buffer is freed.
Logger::~Logger() { sink = g_name.data()[0]; }

int main() {
  (void)g_w;
  return 0;
}
