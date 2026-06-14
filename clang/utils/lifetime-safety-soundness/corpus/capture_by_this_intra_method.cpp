// DESC: a [[clang::lifetime_capture_by(this)]] setter stashes a borrow of a
// local into a member; the capture, the local's destruction, and the read all
// happen inside one method of an object that outlives the local (here a global).
// The capture is modeled as a flow into the whole-object `this` origin, which is
// a caller-scope placeholder that never expires, so the captured local's expiry
// was never reconciled with it -> silent. Keeping `this` live at function exit
// makes the expiry check catch it.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct Latch {
  std::string_view last;
  void set(std::string_view sv [[clang::lifetime_capture_by(this)]]) {
    last = sv;
  }
  void demo() {
    {
      std::string tmp = "a long heap string value exceeding the sso buffer now!!";
      set(tmp); // 'last' now borrows tmp
    }           // tmp destroyed
    volatile char c = last.empty() ? 0 : last[0]; // heap-use-after-free
    (void)c;
  }
};

Latch g;

int main() {
  g.demo();
  return 0;
}
