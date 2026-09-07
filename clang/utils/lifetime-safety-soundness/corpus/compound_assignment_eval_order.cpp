// DESC: a built-in COMPOUND assignment reads its left operand's subexpressions after the right
// operand has already run. C++17 [expr.ass]/1 sequences the right operand before the left for
// every assignment operator, simple and compound alike, and codegen honours it -- so in
// `counts[*k] += weightFor(keys)` the call runs first, reallocating the vector, and `*k` is then
// read through a freed buffer.
//
// The CFG disagreed. CompoundAssignOperator is a distinct StmtClass deriving from
// BinaryOperator, and the CFGBuilder dispatch switches had no case for it, so it fell through to
// the generic default path (VisitChildren over reverse_children) which emits the LHS before the
// RHS. Simple assignment, which does reach the isAssignmentOp() branch, was ordered correctly --
// so `arr[*p] = grow(v)` was caught while `arr[*p] += grow(v)` was silent.
//
// The inversion was symmetric, which is what pins it to the ordering rather than to a missing
// check: `arr[grow(v)] += *p` is SAFE at runtime (the read happens first) and was reported --
// a false positive that disappears with the same fix.
//
// Fixed upstream by 5104dad7a15a, cherry-picked here.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

static std::vector<int> counts(8, 0);

// Reallocates `keys` and returns a weight. Innocuous-looking as an argument.
static int weightFor(std::vector<int> &keys) {
  keys.push_back(1);
  return 1;
}

static int firstWeighted(std::vector<int> &keys) {
  const int *k = &keys[0];       // borrow of the buffer
  counts[*k] += weightFor(keys); // the call runs FIRST, then `*k` is read
  return counts[0];
}

int main() {
  std::vector<int> keys(4, 0);
  sink = firstWeighted(keys);
  return 0;
}
