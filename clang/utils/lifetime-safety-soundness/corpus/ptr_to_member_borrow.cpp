// DESC: `&(s.*pm)` (pointer-to-data-member access via `.*`) names a member of s,
// so it borrows s -- but BO_PtrMemD/BO_PtrMemI were not modeled in
// VisitBinaryOperator, so the borrow of s was dropped to an empty origin.
// Normally lost-loan catches that; here a control-flow merge assigning a valid
// loan (`out = &g`) on the other path masks lost-loan -> silent
// stack-use-after-scope. The member-pointer access now flows the object operand.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-scope
int g = 1;
struct S { int x; };
volatile int sink;
__attribute__((noinline)) int test(bool c) {
  const int *out = &g;          // valid mask loan
  if (c) {
    S s{5};
    int S::*pm = &S::x;
    out = &(s.*pm);             // borrow of s -- dropped without modeling .*
  }                             // s expires
  return sink = *out;          // c==true: dangling read of s.x
}
int main() { return test(true); }
