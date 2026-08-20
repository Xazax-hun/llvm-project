// DESC: a borrow invalidated and then RETURNED, so the caller receives a
// dangling pointer. The analysis detected this all along -- the borrow is live
// across the invalidation and holds the invalidated loan -- but the reporting
// arm for "invalidated, and the escape keeping it live is a return" was an
// unimplemented FIXME, so the finding was computed and then dropped. Which
// spelling reaches that arm is decided by evaluation order: with the borrow READ
// before the invalidating call and deposited into the returned value after it,
// no Use fact spans the call and the return escape is the only thing keeping the
// loan live. The same body written as two statements keeps a Use across the call
// and was reported through the use path, so the two spellings disagreed.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

int *head(std::vector<int> &v [[clang::lifetimebound]], unsigned long cap) {
  int *base = v.data();
  // reserve() reallocates after base has been read, but before the `+`
  // deposits it into the returned value.
  return base + (v.reserve(cap), 0u);
}

int main() {
  std::vector<int> v;
  v.push_back(1);
  int *d = head(v, 1024);
  sink = *d; // heap-use-after-free
  return 0;
}
