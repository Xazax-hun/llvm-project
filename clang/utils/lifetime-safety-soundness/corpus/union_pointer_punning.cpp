// DESC: a borrow stored into and read back through a union member dangles
// EXPECT-ASAN: stack-use-after-scope
union U {
  int *p;
  long i;
};
int main() {
  int *q;
  {
    int x = 5;
    U u;
    u.p = &x; // store the borrow through the union
    q = u.p;  // read it back
  }
  return *q; // x is dead
}
