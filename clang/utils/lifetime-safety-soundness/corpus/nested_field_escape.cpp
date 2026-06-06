// DESC: a borrow of a local escapes to a nested (field-of-field) member
// EXPECT-ASAN: stack-use-after-return
struct Inner {
  int *p;
};
struct Outer {
  Inner a;
  void stash(int *q) { a.p = q; } // q (a local of the caller) escapes to a.p
};
Outer g;
int *leak() {
  int x = 5;
  g.stash(&x);
  return g.a.p; // returns &x, which is dead after leak() returns
}
int main() { return *leak(); }
