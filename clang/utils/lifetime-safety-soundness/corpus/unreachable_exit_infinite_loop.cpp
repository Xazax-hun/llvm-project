// DESC: a borrow used only inside an infinite loop. Liveness is a BACKWARD analysis
// seeded at the CFG's exit block and walked through predecessors, so it only reached
// code that can REACH the exit. An event loop has no edge to the exit, so its blocks
// were left with no state at all and the use inside was invisible -- and liveness is
// what the expiry check intersects against, so the expired borrow looked dead and the
// dangling read went unreported. Not conservatism: zero diagnostics of any kind.
// Same shape for `while (true)`, a backward `goto`, and a trailing [[noreturn]] call.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char uart_tx;

int main() {
  std::string_view banner;
  {
    std::string cfg = "firmware build 2026-09-02 (long enough to heap-allocate)";
    banner = cfg;
  }                      // cfg's buffer is freed here
  for (;;) {             // no CFG edge to the exit block
    uart_tx = banner[0]; // heap-use-after-free
  }
}
