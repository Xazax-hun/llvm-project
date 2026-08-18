// DESC: a borrow of a local passed through the C variadic ellipsis (`...`) to an
// unannotated variadic callee was completely untracked: every borrow/annotation
// guard keys off paramForArg, which returns null for any argument beyond the
// declared parameters, so handleUnannotatedIndirectionArgs skipped it. The
// borrow escaped silently. A variadic-slot argument that can hold a borrow is
// now flagged (-Wlifetime-safety-unannotated-indirection) at the call site.
// EXPECT-ASAN: stack-use-after-return
#include <cstdarg>
#include <cstdio>
const int *g_stash = nullptr;
__attribute__((noinline)) void vstore(int n, ...) {
  va_list ap;
  va_start(ap, n);
  g_stash = va_arg(ap, const int *); // stash the borrow in a global
  va_end(ap);
}
__attribute__((noinline)) void oops() {
  {
    int local = 0xABCD;
    vstore(1, &local); // borrow of inner-block local escapes via `...`
  }                    // local dies
  volatile int filler[64];
  for (int i = 0; i < 64; i++)
    filler[i] = i;
}
int main() {
  oops();
  printf("%d\n", *g_stash); // reads the dead local
  return 0;
}
