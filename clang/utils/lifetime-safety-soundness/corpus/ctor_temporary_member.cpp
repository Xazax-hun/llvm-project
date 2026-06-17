// DESC: member access of a CONSTRUCTOR temporary of a borrow-holding non-gsl
// record (`Box(&x).p`, where `struct Box { int* p; }` has a
// lifetime_capture_by(this) constructor) drops the captured borrow
// (capture_by-on-ctor is unmodeled) and is covered by neither the local-decl
// nor the call-result unknown-ownership check. Its only backstop was lost-loan
// on the dropped borrow, masked here by a control-flow merge whose other path
// stores a valid (non-expiring) loan into the same global -> silent
// stack-use-after-scope. The constructor temporary is now flagged
// unknown-ownership directly.
// EXPECT-ASAN: stack-use-after-scope
struct Box {
  int *p;
  Box(int *q [[clang::lifetime_capture_by(this)]]) : p(q) {}
};
int g_real = 100;
int *gp;
__attribute__((noinline)) void f(bool c) {
  if (c) {
    int x = 42;
    gp = Box(&x).p; // borrow of x dropped; masked by the other path's loan
  } else {
    gp = &g_real;
  }
  *gp = 7; // c==true: writes destroyed x
}
int main() {
  f(true);
  return 0;
}
