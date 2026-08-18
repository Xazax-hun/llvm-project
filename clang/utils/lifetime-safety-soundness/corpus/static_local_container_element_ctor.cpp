// DESC: a container as a function-local STATIC inside a verified destructor. A static
// local is constructed when control first reaches its declaration -- and when that
// declaration is inside shutdown code, that is when its elements' constructors run.
//
// The construction check deliberately skipped non-automatic storage, deferring to the
// walk over static-duration variables. But that walk judges DESTRUCTION: it asks whether
// destroying the object at shutdown can observe another already destroyed. For
// `std::vector<Logger>` the answer is yes-it-is-safe, because `Logger`'s destructor is
// trivial. Nothing on either path asked what CONSTRUCTING it runs, so the two checks
// each correctly answered a question that was not the dangerous one.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <vector>

volatile char sink;

struct Logger {
  Logger();
};

struct [[clang::destruction_order_safe]] A {
  ~A();
};

A a;                    // constructed first  -> destroyed LAST
std::string g_name = "a-global-string-long-enough-that-its-buffer-is-heap-allocated";

Logger::Logger() { sink = g_name.data()[0]; }

// First reached during shutdown, so this is where the element is constructed.
A::~A() { static std::vector<Logger> v(1); }

int main() { return 0; }
