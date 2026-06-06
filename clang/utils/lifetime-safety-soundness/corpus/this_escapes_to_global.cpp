// DESC: a method leaks 'this' to a global; the object then dies (the global
// store of a caller-scope borrow is not flagged intra-procedurally)
// EXPECT-ASAN: stack-use-after-scope
struct S {
  int f = 7;
  void reg();
};
S *g;
void S::reg() { g = this; } // 'this' (caller-scope) escapes to a global

int main() {
  static S keep;
  g = &keep; // give 'g' a valid loan so a lost-loan net cannot mask the bug
  {
    S s;
    s.reg(); // unmodeled at the call site: 'g' now points to the soon-dead 's'
  }
  return g->f; // g points to the destroyed 's'
}
