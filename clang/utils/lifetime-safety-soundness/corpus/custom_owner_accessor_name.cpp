// DESC: the non-invalidating read-accessor allow-list (data/find/...) leaked out
// of the genuine std namespace via the reserved-identifier heuristic (`__`/
// `_Uppercase` namespaces treated as STL). A custom [[gsl::Owner]] in a `__`-
// namespace with a non-const, reallocating method merely NAMED `data` was wrongly
// exempted from assumed invalidation, so a borrow taken before the call dangled
// silently. The exemption now requires a genuine `std` ancestor, so the custom
// type's `data()` is assumed-invalidating.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>
namespace __detail {
struct [[gsl::Owner(int)]] Buf {
  std::vector<int> v{1, 2, 3};
  const int *begin() const [[clang::lifetimebound]] { return v.data(); }
  int data() { v.push_back(99); return 0; } // reallocates; name on allow-list
};
} // namespace __detail
volatile int sink;
int main() {
  __detail::Buf b;
  const int *p = b.begin(); // borrow into b.v's heap buffer
  b.data();                 // reallocates b.v -> p dangles
  sink = *p;                // heap-use-after-free
  return 0;
}
