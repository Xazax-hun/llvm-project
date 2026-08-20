// DESC: a compound assignment whose RHS invalidates what the LHS borrows. A
// compound assignment reads AND writes its left operand, and both happen AFTER
// the right operand is evaluated -- so `p[0] += v.emplace_back(5)` reallocates the
// vector and then writes through the stale `p`. The LHS's own use is registered
// when the LHS is evaluated, which is before the RHS, so nothing kept the borrow
// live across the invalidation and the read-modify-write was not modelled at all.
// The plain-assignment spelling `p[0] = v.emplace_back(5)` WAS reported, but only
// because C++17 sequences its RHS before its LHS, which puts the LHS use after the
// invalidation by luck of the evaluation order rather than by anything modelling
// the write -- so the two spellings of one hazard disagreed, and every compound
// operator (+=, -=, *=, |=, <<=, ...) was silent.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int isink;

int main() {
  std::vector<int> v{1, 2, 3};
  int *p = &v[0];
  p[0] += v.emplace_back(5); // reallocates v, then writes through the stale p
  isink = v[0];
  return 0;
}
