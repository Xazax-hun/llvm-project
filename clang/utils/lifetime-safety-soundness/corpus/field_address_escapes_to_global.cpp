// DESC: a method leaks the address of a field to a global; the object then dies
// (the global store of a caller-scope field borrow is not flagged)
// EXPECT-ASAN: stack-use-after-scope
int *g;
struct S {
  int f = 7;
  void reg() { g = &f; } // &this->f (caller-scope) escapes to a global
};

int main() {
  static int keep = 1;
  g = &keep; // valid loan so a lost-loan net cannot mask the bug
  {
    S s;
    s.reg(); // unmodeled at the call site: 'g' now points into the dead 's'
  }
  return *g; // reads the destroyed field
}
