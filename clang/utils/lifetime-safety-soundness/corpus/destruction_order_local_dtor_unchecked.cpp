// DESC: a '[[clang::destruction_order_safe]]' destructor that runs an unverified
// destructor by creating a local. Destroying an object is never a `CallExpr` -- there
// is no AST node for the implicit destruction of a local, a temporary, or the target
// of `delete` -- so the body checker, which looked at calls, saw nothing at all. The
// promise was satisfiable while arbitrary unchecked code ran at shutdown.
//
// Note the asymmetry that made this easy to miss: `Peek` as a MEMBER of the annotated
// type was reported (the record-level walk checks bases and members), and an explicit
// constructor call was reported, but a local's destruction was invisible. The fix tests
// the type directly at the point the object is created, mirroring the member check.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

extern std::string g_str;

// Not annotated, so not known to be safe -- and it is not: it reads a global that
// may already have been destroyed.
struct Peek {
  ~Peek() { sink = g_str[0]; }
};

struct [[clang::destruction_order_safe]] Reader {
  ~Reader() {
    Peek p; // its destructor runs here, at shutdown, unchecked
    (void)p;
  }
};

Reader g_reader; // registered first -> destroyed LAST

std::string g_str = "0123456789012345678901234567890123456789012345678901234567890123456789";

int main() { return 0; }
