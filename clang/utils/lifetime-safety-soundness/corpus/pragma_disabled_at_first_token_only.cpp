// DESC: whether a function is analyzed at all was decided from the diagnostic
// state at its FIRST TOKEN only. A pragma region that ends inside the body
// re-enables the warnings for the rest of it, but the analysis had already been
// skipped, so the dangling use below (outside every pragma) went unreported.
// EXPECT-ASAN: stack-use-after-scope
volatile int sink;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-soundness"
void f() {
#pragma clang diagnostic pop
  int *p;
  { int x = 1; p = &x; }
  sink = *p;
}
int main() { f(); }
