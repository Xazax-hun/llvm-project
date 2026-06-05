// DESC: returning the address of a member of a local object
// EXPECT-ASAN: stack-use-after-return
struct S {
  int a;
  int b;
};
__attribute__((noinline)) int *member() {
  S s{1, 2};
  return &s.b;
}
int main() { return *member(); }
