// DESC: the class-level '[[clang::destruction_order_safe]]' attribute used as an escape
// hatch. Trusting every function of an annotated class -- rather than only its
// destructor -- made the promise satisfiable by construction: annotate the class, then
// put the global access in any non-destructor member. Two lines, no cleverness.
//
// The class attribute is a promise about DESTRUCTION: that the type may hold static
// storage duration and that its destructor is verified. It says nothing about the rest
// of the interface. A member that can run during shutdown -- because a verified body
// calls it -- has to carry the attribute itself, which is what puts its own body
// through the check. The same applied to constructors and static member functions.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

extern std::string g_str;

struct [[clang::destruction_order_safe]] Reader {
  // Trusted on the class's say-so, and never verified.
  char helper() const { return g_str[0]; }
  ~Reader() { sink = helper(); }
};

Reader g_reader; // registered first -> destroyed LAST

std::string g_str = "0123456789012345678901234567890123456789012345678901234567890123456789";

int main() { return 0; }
