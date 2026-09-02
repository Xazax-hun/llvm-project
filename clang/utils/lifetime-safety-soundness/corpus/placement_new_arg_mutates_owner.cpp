// DESC: a placement-new ARGUMENT reallocates the owner a live borrow points into. A
// placement argument is an ordinary argument as far as what the callee may do to it, but
// spelling the call as a new-expression routes it through VisitCXXNewExpr instead of
// handleFunctionCall -- and only the unannotated-indirection question was re-asked there, so
// the mutation went unmodelled. Reduced to three functions differing ONLY in call syntax:
// `::operator new(sizeof(int), Tag{}, v)` was caught, an identically-signed plain function
// `myAlloc(sizeof(int), Tag{}, v)` was caught, and `new (Tag{}, v) int(3)` was silent. Also
// silent when the mutating argument is the FIRST placement argument, and for `new[]`. The
// placement argument the result points INTO is excluded from the mutation question: `new
// (buf) T` writes bytes into buf, it does not reallocate it, and its parameter is typically
// `void *` which is otherwise assumed mutable (the opaque-userdata idiom).
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>
#include <cstddef>

volatile int g_sink;
struct Tag {};

void *operator new(std::size_t n, Tag, std::vector<int> &v [[clang::noescape]]) {
  v.push_back(1);
  return new char[n];
}

int main() {
  std::vector<int> v;
  v.push_back(0);
  int *p = &v[0];
  int *q = new (Tag{}, v) int(3);   // reallocates v
  g_sink = *p;                      // UAF
  (void)q;
  return 0;
}
