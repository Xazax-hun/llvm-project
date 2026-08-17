// DESC: an ANNOTATED type holding a container of an unsafe element. The container rule asks
// whether a std specialization's element constructors run unverified user code, because that
// call is made from inside the library where no verified body can see it. Skipping that
// question for a type that carries '[[clang::destruction_order_safe]]' looked reasonable --
// the promise is verified, after all -- but the verification walks the constructor's
// initializers and arrives at the member container's own constructor, which it trusts as
// library code. So the one check that could see `Inner::Inner()` was the one being skipped,
// and annotating the WRAPPER hid exactly what the rule exists to catch.
//
// The promise covers the class's own constructor, not what a member container constructs. So
// the container question is asked of an annotated type's bases and members too; only the
// class's own user-written constructor is taken as verified.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <vector>
volatile char sink;
extern std::string buf;
struct Inner { char c; Inner(); };
// Annotated, so the container rule was skipped -- but its verified ctor body cannot
// see inside libc++, which is the very reason that rule exists.
struct [[clang::destruction_order_safe]] W6 {
  std::vector<Inner> v;
  [[clang::destruction_order_safe]] W6() : v(1) {}
};
struct [[clang::destruction_order_safe]] G {
  ~G() { W6 f; sink = f.v[0].c; }
};
G g;
std::string buf = std::string(200, 'x');
Inner::Inner() : c(buf[0]) {}
int main() { return 0; }
