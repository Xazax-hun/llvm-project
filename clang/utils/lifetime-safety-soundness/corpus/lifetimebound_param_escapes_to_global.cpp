// DESC: a [[clang::lifetimebound]] parameter also escapes to a global; the
// annotation documents only the return binding, not the global capture, so the
// caller is unaware the global now aliases the argument
// EXPECT-ASAN: stack-use-after-scope
int *g;
int *pick(int *a [[clang::lifetimebound]]) {
  g = a; // 'a' escapes to a global -- not covered by lifetimebound
  return a;
}

int main() {
  static int keep = 1;
  g = &keep; // valid loan so a lost-loan net cannot mask the bug
  {
    int x = 2;
    pick(&x); // caller only learns the (discarded) return is bound to x
  }
  return *g; // g points to the destroyed x
}
