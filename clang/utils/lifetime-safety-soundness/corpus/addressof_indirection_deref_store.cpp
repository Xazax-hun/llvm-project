// DESC: a borrow is stored into a view via `*&sv = tmp` (a store whose LHS is a
// dereference). `*&X` is exactly `X`, but the store form was unmodeled
// (handleAssignment only resolved DeclRefExpr/MemberExpr/array-element LHSs), so
// the store was dropped and `sv` kept its stale loan into the still-live
// `valid` while its bytes were overwritten to point into the freed `tmp`. Found
// by the 6th multi-agent bypass hunt (E1). Closed soundly by banning `&p` where
// `p` is an indirection (a transient double indirection), mirroring the
// declaration-level single-indirection rule -- `*&sv` forms `&sv`.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

int leak() {
  std::string valid = "valid backing string content for the analysis to use!!";
  std::string_view sv = valid; // sv borrows valid
  {
    std::string tmp(64, 'X');
    *&sv = tmp; // store-through-deref: sv now points into tmp
  }             // tmp freed; sv dangles
  return sv.size() ? sv[0] : 0; // use-after-free
}

int main() {
  volatile int r = leak();
  return r;
}
