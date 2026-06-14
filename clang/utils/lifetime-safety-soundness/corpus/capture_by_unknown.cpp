// DESC: a setter annotated [[clang::lifetime_capture_by(unknown)]] stashes a
// borrow of a local into a member; the local then dies and the member is read.
// capture_by(unknown) modeled nothing (it silenced the call-site annotation
// requirement while tracking no flow), so the dangling capture was silent. The
// safe model now bans capture_by(unknown), like capture_by(global).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct Latch {
  std::string_view last;
  void set(std::string_view sv [[clang::lifetime_capture_by(unknown)]]) {
    last = sv;
  }
  void demo() {
    {
      std::string tmp = "this is a long heap string that will be freed soon!!";
      set(tmp); // 'last' now borrows tmp
    }           // tmp destroyed
    volatile char c = last.empty() ? 0 : last[0]; // heap-use-after-free
    (void)c;
  }
};

Latch g; // non-local instance

int main() {
  g.demo();
  return 0;
}
