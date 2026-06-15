// DESC: a borrow used via a pointer COMPOUND-ASSIGNMENT result (`g = (p += 5)`)
// escaped to a global silently: VisitBinaryOperator early-returned for any
// compound-assignment op, so the `+=` result origin carried no loan and the
// dangling-global escape diagnostic was dropped (pre/post inc-dec on a pointer
// had the same gap in VisitUnaryOperator). These operators now propagate the
// operand's loans, so the escape -- and a downstream use -- is tracked.
// EXPECT-ASAN: heap-use-after-free
#include <string>

const char *g;

void f() {
  std::string local = "0123456789abcdefghijklmnopqrstuvwxyz a long heap buffer!";
  const char *p = local.data(); // p borrows local's heap buffer
  g = (p += 5);                 // escape the borrow via the compound-assign result
}                               // local freed -> g dangles

volatile char sink;

int main() {
  f();
  sink = g[0]; // heap-use-after-free
  return 0;
}
