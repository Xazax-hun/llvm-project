// DESC: a [[gsl::Pointer]] view with a non-trivial destructor that reads its
// captured borrow is declared BEFORE the local it borrows, so the view is
// destroyed LAST (reverse construction order) and its destructor reads the
// already-destroyed local. The CFG builder batched the trivial local's
// lifetime-end after the view's destructor, so the analysis thought the local
// was still alive at destruction -> silent. Now the CFG interleaves trivial and
// non-trivial cleanups in reverse construction order.
// EXPECT-ASAN: stack-use-after-scope
#include <cstdio>
struct [[gsl::Pointer(int)]] View {
  const int *p;
  __attribute__((noinline)) ~View() {
    if (p) { volatile int t = *p; printf("%d\n", t); }
  }
};
__attribute__((noinline)) void f() {
  View v{nullptr};   // declared first  -> destroyed LAST
  int a = 12345;     // declared second -> destroyed FIRST
  v = View{&a};      // v borrows a; ~View() reads a after it is gone
}
int main() {
  f();
  return 0;
}
