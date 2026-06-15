// DESC: a borrow into one union member (u.a) is invalidated by reallocating a
// DIFFERENT, aliasing union member (u.b) -- both occupy the same storage. The
// analysis keys field-mutation invalidation by FieldDecl identity and has no
// notion that two union members alias, so the borrow was not invalidated. The
// safe model now rejects union member access outright (type punning the
// analysis cannot reason about).
// EXPECT-ASAN: heap-use-after-free
#include <vector>

union U {
  std::vector<int> a;
  std::vector<int> b;
  U() : a(std::vector<int>(100, 7)) {}
  ~U() { a.~vector(); }
};

volatile int sink;

int main() {
  U u;
  const int &r = u.a[0]; // borrow into union member 'a'
  u.b.push_back(9);      // mutate via member 'b' (same storage) -> reallocation
  sink = r;              // heap-use-after-free
  return 0;
}
