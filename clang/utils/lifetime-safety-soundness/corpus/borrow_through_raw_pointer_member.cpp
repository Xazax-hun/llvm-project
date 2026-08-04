// DESC: a borrow taken through a raw pointer MEMBER, invalidated by a mutation
// through that same member. The facts were correct and checkInvalidation matched
// the borrow -- the report was then dropped. Every pointer/view member of `this`
// is seeded at entry with a non-expiring "uninitialized" loan (no issuing
// expression, AccessPath::Kind::Uninitialized), so it has neither anchor the
// escape-caused report branches handle. FinalWarningsMap is keyed by loan, and the
// member's own origin -- live at function exit, hence an escape-caused entry --
// claimed the slot first with a dominating causing fact, then emitted nothing,
// masking the use-caused entry for the same loan, which does have a use anchor.
// A reportable entry now takes over from a dominating unreportable one.
// Note [[gsl::Owner]] here is FORCED by the model: without it, Box trips
// -Wlifetime-safety-unknown-ownership, so this is the shape safe-model code must
// adopt. The same bug with a LOCAL pointer, or with a unique_ptr member, was
// already reported -- only the raw-pointer-member form slipped.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

struct [[gsl::Owner]] Box {
  Box() : pv(new std::vector<int>{1, 2, 3}) {}
  ~Box() { delete pv; }

  int bad() {
    int *q = pv->data(); // borrow into *pv
    pv->push_back(99);   // reallocates -> q dangles
    return *q;           // heap-use-after-free
  }

private:
  std::vector<int> *pv; // raw pointer member: the seeded loan has no anchor
};

int main() {
  Box b;
  sink = b.bad();
  return 0;
}
