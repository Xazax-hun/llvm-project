// DESC: a struct whose only indirection-holding member is a C-ARRAY of raw
// pointers (`int* arr[1]`) was not recognized as a borrow-holding (unknown-
// ownership) type: RecordHoldsBorrow / isUnknownOwnershipType did not peel the
// array dimension, so a ConstantArrayType member (not pointer-like, null
// getAsCXXRecordDecl) hid the borrow. make() returning Holder{{&x}} stored the
// address of a local into arr[0] and was silent (no named local to look lost,
// the struct treated as a clean value). The recognition checks now peel array
// dimensions on the field type.
// EXPECT-ASAN: stack-use-after-return
#include <cstdio>
struct Holder { int *arr[1]; };
__attribute__((noinline)) Holder make(int x) { return Holder{{&x}}; }
int main() {
  Holder h = make(7);
  printf("%d\n", *h.arr[0]); // reads the returned stack slot of make()'s x
  return 0;
}
