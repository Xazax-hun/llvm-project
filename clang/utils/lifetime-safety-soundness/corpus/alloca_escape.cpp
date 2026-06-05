// DESC: a pointer into alloca'd storage escapes the function that allocated it
// EXPECT-ASAN: stack-use-after-return
#include <alloca.h>
static int *make() {
  int *p = (int *)alloca(sizeof(int)); // freed when make() returns
  *p = 5;
  return p;
}
int main() {
  int *q = make();
  return *q; // q points into make()'s freed stack frame
}
