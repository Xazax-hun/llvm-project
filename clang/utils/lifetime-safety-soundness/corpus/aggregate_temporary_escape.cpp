// DESC: a plain (non-gsl) aggregate that holds a borrow member is brace-
// initialized as a TEMPORARY whose member escapes to a global (the equivalent
// return form dangles too). handleGslAggregateInit only models
// gsl::Pointer/Owner aggregates, so the captured borrow was orphaned; the
// unknown-ownership backstop only fired on a local VarDecl or a call result, not
// on an escaping InitListExpr temporary -> silent. The aggregate temporary is
// now flagged unknown-ownership like the local-declaration form.
// EXPECT-ASAN: stack-use-after-return
#include <cstdio>
struct Box { int *p; int n; };
int *g;
__attribute__((noinline)) void store() {
  int x = 5;
  g = Box{&x, 0}.p; // temporary aggregate's member escapes; x dies on return
}
int main() {
  store();
  volatile int v = *g; // read dangling stack address
  printf("%d\n", v);
  return 0;
}
