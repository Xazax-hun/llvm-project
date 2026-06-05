// DESC: a member function called through a deleted object
// EXPECT-ASAN: heap-use-after-free
struct S {
  int v = 3;
  int read() { return v; }
};
int main() {
  S *s = new S;
  delete s;
  return s->read();
}
