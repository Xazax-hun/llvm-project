// DESC: the destruction-order body verifier defeated by one dependent call. A
// '[[clang::destruction_order_safe]]' destructor may only call functions that carry the
// promise too, and calling `helper()` directly from `~Logger` is correctly refused. Routing
// it through a GENERIC lambda erased the requirement entirely.
//
// Two things combined. A generic lambda's call operator is a function template minted inside
// an expression, and the walk reaches instantiated bodies through the specializations of the
// templates it enumerates -- which are the ones declared at file or class scope -- so only
// the dependent PATTERN was ever seen. And the body checker exempted a lambda written inside
// the function being verified, on the grounds that "its body is written here and is traversed
// with it". That is true of an ordinary lambda but false of a generic one: what is written
// here is a pattern, in which `decltype(tag)::helper()` resolves to nothing.
//
// So the promise stayed checked while its verification became optional -- the worst kind of
// hole in an annotation-based rule, since the annotation still looks enforced.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

extern std::string g_str;

struct H {
  static void helper();
};

struct [[clang::destruction_order_safe]] Logger {
  ~Logger() {
    // The dependent call is invisible in the pattern, and the instantiation was never
    // visited. Calling H::helper() directly here is refused.
    auto g = [](auto tag) { decltype(tag)::helper(); };
    g(H{});
  }
};

Logger g_log;                                       // dyn-init #1 -> destroyed LAST
std::string g_str = std::string(70, 'd');            // #2 -> destroyed FIRST

void H::helper() { sink = g_str[0]; }                // heap-use-after-free

int main() { return 0; }
