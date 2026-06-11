// DESC: a raw pointer borrows INTO a mutable global owner (`&g[0]`), then that
// global is mutated (reallocated) -- here from inside a function the pointer is
// passed to as a [[clang::noescape]] parameter. The borrow into the global is
// only visible as a loan at the call site (severed to a placeholder inside the
// callee). The "cannot borrow from a mutable global" rule previously fired only
// for GSL view construction, missing the raw-pointer-into-subscript form. Found
// by the 3rd multi-agent bypass hunt (B1).
// EXPECT-ASAN: heap-use-after-free
#include <vector>

std::vector<int> g = {1, 2, 3};

int use_after_grow(int *elem [[clang::noescape]]) {
  g.push_back(7777); // reallocates the global g -> elem dangles
  return *elem;      // use-after-free
}

int sink;
int main() {
  sink = use_after_grow(&g[0]); // borrow into the mutable global g
  return sink;
}
