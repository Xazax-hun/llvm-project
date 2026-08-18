// DESC: a hazard written wholly inside a DEFAULT ARGUMENT. A default argument is code that
// runs whenever a call omits it, but no analysis entry point reached it: the CFG
// deliberately does not descend into a `CXXDefaultArgExpr`, because the expression belongs
// to the callee's declaration and adding it to each caller's CFG would make one Expr appear
// in several (PR13385), and the per-function path analyzes bodies, which this is not.
//
// The identical expression written at the call site is reported, which is what localizes
// this to the missing entry point rather than to the invalidation reasoning.
//
// The parameter type matters: with a tracked type -- `std::string_view`, `const
// std::string &` -- the borrow reaches an origin and the lost-loan sentinel fires anyway.
// Consuming the borrow into a plain `int` leaves nothing for that backstop to notice, which
// is why this shape needed the default argument to be analyzed in its own right.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

std::string *gp = nullptr;
std::string_view gsv;

// Borrows from the heap string, frees it, then reads through the dangling view -- all
// inside the default argument, and all consumed into an `int`.
void f(int x = (gp = new std::string(72, 'a'), gsv = std::string_view(*gp), delete gp,
                gsv[0])) {
  sink = (char)x;
}

int main() {
  f();
  return 0;
}
