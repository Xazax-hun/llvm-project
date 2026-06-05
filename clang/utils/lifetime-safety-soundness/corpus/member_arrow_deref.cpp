// DESC: '->' dereference of a dangling pointer to a local object
// EXPECT-ASAN: stack-use-after-scope
struct S {
  int v;
};
int main() {
  S *p;
  {
    S s{42};
    p = &s;
  }
  return p->v;
}
