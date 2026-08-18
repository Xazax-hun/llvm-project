// DESC: a store into an inherited member through a derived-to-base cast of `this`
// (`static_cast<Base*>(this)->p = &local`) was not recognized as a this-member
// store -- the recognition required a direct CXXThisExpr base and an explicit
// cast is not stripped by IgnoreParenImpCasts -- so the borrow routed to a fresh
// disconnected origin and was dropped, leaving the dangling field read silent.
// `this`-member recognition now strips derived-to-base/no-op casts of this.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-return
struct Base {
  const int *p = nullptr;
  __attribute__((noinline)) int read() const { return *p; }
};
struct Derived : Base {
  __attribute__((noinline)) void stash() {
    int local = 42;
    static_cast<Base *>(this)->p = &local; // store through base cast of this -- dropped
  }
};
volatile int sink;
int main() {
  Derived d;
  d.stash();
  { volatile int junk[64]; for (int i = 0; i < 64; ++i) junk[i] = i; (void)junk; }
  sink = d.read(); // dangling read
  return 0;
}
