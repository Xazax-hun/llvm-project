// DESC: the unknown-ownership / owner-of-indirection checks bailed on array types
// (getAsCXXRecordDecl() is null for an array), so a local array of a plain
// borrow-holding struct was flagged at neither its declaration nor any element
// access -- the captured borrow was dropped and the array's Uninitialized
// element-origin sentinel kept lost-loan silent. Array dimensions are now peeled,
// so the array is flagged like the scalar `P a;`.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-scope
struct P { const int *p; };
volatile int sink;
__attribute__((noinline)) int f() {
  P a[1] = {{nullptr}};
  {
    int local = 7;
    a[0] = P{&local};   // aggregate store captures &local into the array element
  }                     // local dies -> a[0].p dangles
  return sink = *a[0].p; // use-after-scope
}
int main() { return f(); }
