// DESC: the same root cause with the hazard BEFORE the loop. In a function that never
// returns, the CFG's exit block has no predecessors at all, so the backward liveness
// walk terminated immediately and NOTHING in the body was analyzed -- not just the
// loop. An ordinary invalidation (reserve() reallocating a vector out from under a
// borrow) sitting well before the loop was therefore missed too, which makes any
// [[noreturn]] worker, event loop, or scheduler body a blind spot in its entirety.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

[[noreturn]] static void serve() {
  std::vector<int> buf(4);
  buf[0] = 7;
  int *p = &buf[0];
  buf.reserve(4096); // reallocates -> p dangles
  sink = *p;         // heap-use-after-free
  for (;;) {
  }
}

int main() { serve(); }
