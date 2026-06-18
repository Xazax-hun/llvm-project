// DESC: an inline-asm output operand ("=r"(p)) reseats a pointer at runtime, but
// the fact generator had no handler for asm statements, so it modeled neither a
// kill of p's loans nor a flow of the input borrow -- p silently kept its prior
// (stale, long-lived) loan, which masked lost-loan, while the asm actually
// pointed it at a now-dead local. Inline assembly is opaque (an output can
// reseat a pointer; an input/memory clobber can move or invalidate a borrow), so
// it is now rejected under the safe programming model.
// EXPECT-ASAN: stack-use-after-scope
#include <cstdio>
int g_sink = 0;
int g_valid = 0;
__attribute__((noinline)) void f() {
  int *p = &g_valid; // stale valid loan masks lost-loan
  {
    int local = 42;
    asm("mov %0, %1" : "=r"(p) : "r"(&local)); // reseat p to &local (unmodeled)
  }
  g_sink = *p; // reads the dead 'local'
}
int main() {
  f();
  printf("%d\n", g_sink);
  return 0;
}
