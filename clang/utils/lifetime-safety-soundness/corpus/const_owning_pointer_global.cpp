// DESC: `const` on a global excludes it from the borrow-from-mutable-global rule,
// because a const owner cannot be reallocated. That holds for
// `const std::vector<int>`, but not for an owning smart pointer: `const` applies
// to the pointer, not to what it owns. `const std::unique_ptr<std::vector<int>>`
// still hands out a non-const `std::vector<int>*` from its const `operator->`, so
// `g->push_back(7)` compiles and reallocates the vector a caller holds a borrow
// into. One `const` silenced the rule for every owning smart pointer. The mutation
// sits in a separate function here, so the intra-procedural invalidation check
// cannot see it and the mutable-global rule is the only thing that covers it --
// which is what made the exclusion load-bearing. A const pointee
// (`unique_ptr<const vector<int>>`) is still protected and stays excluded.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <vector>

volatile int isink;

static const std::unique_ptr<std::vector<int>> g_p =
    std::make_unique<std::vector<int>>();

// const applies to the pointer; the vector it owns is freely mutable.
static void grow() { g_p->push_back(7); }

int main() {
  grow();
  int *p = &(*g_p)[0]; // borrow into the owned vector's buffer
  grow();              // reallocates it
  isink = *p;          // heap-use-after-free
  return 0;
}
