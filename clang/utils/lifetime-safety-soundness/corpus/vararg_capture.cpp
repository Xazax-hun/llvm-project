// DESC: a borrow to a local escapes through C varargs
// EXPECT-ASAN: stack-use-after-scope
#include <cstdarg>
int *g_p;
__attribute__((noinline)) void capture(int n, ...) {
  va_list ap;
  va_start(ap, n);
  g_p = va_arg(ap, int *);
  va_end(ap);
}
int main() {
  {
    int x = 3;
    capture(1, &x);
  }
  return *g_p;
}
