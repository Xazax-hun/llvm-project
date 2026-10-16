// DESC: an ASSUMED invalidation (a non-const method whose body is not seen) through
// a receiver whose borrow was lost -- std::max on pointers yields only an Unknown
// loan -- invalidated nothing, while the same call on `q` directly was reported.
// The precise invalidation check already treated an Unknown loan as reaching
// everything; the assumed one did not.
// EXPECT-ASAN: heap-use-after-free
#include <algorithm>
#include <vector>
static volatile int sink;
struct W {
  std::vector<int> v;
  void grow();          // body elsewhere: assumed to mutate
  const int *first() const [[clang::lifetimebound]] { return v.data(); }
};
void W::grow() { v.resize(1000); }
int main() {
  W a, b;
  a.v.resize(4); b.v.resize(4);
  W *q = &a;
  const int *p = q->first();
  std::max(q, q)->grow();
  sink = *p;
}
