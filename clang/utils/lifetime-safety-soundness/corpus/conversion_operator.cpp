// DESC: a user conversion operator returns the address of a local's member
// EXPECT-ASAN: stack-use-after-return
struct C {
  int x;
  operator int *() { return &x; }
};
__attribute__((noinline)) int *convert() {
  C c{3};
  return c; // uses C::operator int*() -> &c.x
}
int main() {
  // The borrow is lost through the (unmodeled) conversion operator; the
  // soundness net flags 'p' as holding no tracked loan.
  int *p = convert();
  return *p;
}
