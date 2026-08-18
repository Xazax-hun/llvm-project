// DESC: a library method called on a container of a hazardous element type, from a
// verified destructor. Library code is trusted because it does not name a user's globals
// -- but a container CALLS the constructors of its elements, and `m.emplace_back()` on a
// `std::vector<Logger>` runs `Logger::Logger` during shutdown with nothing in the verified
// body naming it. Trusting `emplace_back` for the header it was written in, without regard
// for what it is parameterized by, is the whole gap.
//
// No construction-site check can reach this one: the container is a MEMBER, constructed
// long before shutdown, so no construction in this body describes it. The question has to
// be asked at the call, and the answer has to chain -- `emplace_back` is safe only if
// `std::vector<Logger>` is, which needs `Logger` to be.
//
// `resize`, `assign` (the element's COPY constructor) and `std::optional::emplace` are the
// same shape. The escape hatch is annotating `Logger`'s constructor, which is the honest
// fix and clears every one of them at once.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <vector>

volatile char sink;

// Trivially destructible, so every rule that asks "is the destructor safe?" passes it --
// and `std::vector<Logger>` inherits that pass. The constructor is the hazard.
struct Logger {
  Logger();
};

struct [[clang::destruction_order_safe]] A {
  std::vector<Logger> m;
  ~A();
};

A a;                    // constructed first  -> destroyed LAST
std::string g_name = "a-global-string-long-enough-that-its-buffer-is-heap-allocated";

// By the time this runs at shutdown, `g_name`'s buffer is already freed.
Logger::Logger() { sink = g_name.data()[0]; }

// The element is constructed inside libc++, where this body cannot see it.
A::~A() { m.emplace_back(); }

int main() { return 0; }
