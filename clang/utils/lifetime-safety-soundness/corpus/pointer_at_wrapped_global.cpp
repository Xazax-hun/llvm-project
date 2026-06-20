// DESC: a raw pointer borrows the WHOLE mutable global wrapper object (`&g`,
// where `g`'s type is a non-owner record containing a `std::vector`), not just
// into its buffer. The caller can mutate the owner through that pointer,
// invalidating a sibling borrow into the same global. The "cannot borrow from a
// mutable global" rule previously excluded a pointer whose pointee was the whole
// object ("points at a stable object"), so a pointer to a wrapper that reaches a
// mutable owner slipped through. Found by the multi-agent bypass hunt (Agent B):
// any borrow extracted from a mutable global owner must be banned, the only
// permitted interaction being a method call on the global itself.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct W {
  std::vector<int> v;
};
W g = {{1, 2, 3}};

void grow_through(W *p) { p->v.push_back(9999); } // reallocates g.v via the alias

int sink;
int main() {
  int *elem = &g.v[0];  // borrow into the global wrapper's owner
  grow_through(&g);     // mutate the global through a pointer to the wrapper
  sink = *elem;         // use-after-free: g.v's buffer moved
  return sink;
}
