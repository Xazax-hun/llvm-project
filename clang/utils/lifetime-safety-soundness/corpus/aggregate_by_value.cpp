// DESC: an aggregate returned by value carries a pointer to a local
// EXPECT-ASAN: stack-use-after-return
struct Holder {
  int *p;
};
__attribute__((noinline)) Holder make() {
  int x = 9;
  return Holder{&x};
}
int main() {
  Holder h = make();
  return *h.p;
}
