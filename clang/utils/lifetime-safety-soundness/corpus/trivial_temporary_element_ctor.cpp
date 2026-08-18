// DESC: a TRIVIALLY DESTRUCTIBLE temporary whose construction runs user code. The
// construction question was asked of a temporary at its CXXBindTemporaryExpr -- a node
// that exists only when the temporary needs destroying. libc++'s `std::optional<T>` is
// trivially destructible exactly when `T` is, so `std::optional<Logger>(std::in_place)`
// produces no such node at all and the temporary was never asked about.
//
// That makes the gap self-reinforcing: the very property that makes a type escape every
// destructor-based rule -- having nothing to destroy -- is also what removes the AST node
// the construction rule hung on. Adding a non-trivial member to `Logger` would have made
// this fire, which is the opposite of what a safety rule should require.
// EXPECT-ASAN: heap-use-after-free
#include <optional>
#include <string>

volatile char sink;

// Trivially destructible, so `std::optional<Logger>` is too.
struct Logger {
  Logger();
};

struct [[clang::destruction_order_safe]] A {
  ~A();
};

A a;                    // constructed first  -> destroyed LAST
std::string g_name = "a-global-string-long-enough-that-its-buffer-is-heap-allocated";

Logger::Logger() { sink = g_name.data()[0]; }

// Constructs a Logger inside the library, in a temporary that needs no destruction.
A::~A() { (void)std::optional<Logger>(std::in_place); }

int main() { return 0; }
