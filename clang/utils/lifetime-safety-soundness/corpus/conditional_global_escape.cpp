// DESC: a borrow to a local escapes into a global on a CONDITIONAL path; the
// store sits on some-but-not-all paths and the local expires at a later merge.
// The escaping global origin was misclassified as block-local (the escape fact
// did not count as a cross-block appearance), so its loan was dropped at the
// join before the escape/expiry check -> silently missed. The unconditional
// form was caught; this conditional form was not.
// EXPECT-ASAN: stack-use-after-return
int *g_p;
__attribute__((noinline)) void store(int c) {
  int x = 5;
  if (c)
    g_p = &x;
}
int main() {
  store(1);
  return *g_p;
}
