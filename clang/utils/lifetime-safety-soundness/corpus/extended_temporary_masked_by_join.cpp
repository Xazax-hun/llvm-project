// DESC: a borrow of a LIFETIME-EXTENDED temporary, masked at a join. A loan was issued for a
// temporary only when its storage duration was SD_FullExpression. Bind the temporary to a
// reference and it becomes SD_Automatic -- lifetime-extended -- and no loan was created at
// all, leaving the reference's origin EMPTY rather than carrying anything.
//
// Two consequences, and the second is what makes this quiet. No expiry could fire, since
// there was no loan to expire. And the `lost-loan` sentinel needs the origin to be entirely
// empty, so any co-resident real loan masks it -- a conditional whose other arm carries a
// tracked borrow is enough. Unmasked, this shape was caught by that sentinel alone, never by
// reasoning about the lifetime.
//
// The control that pins the mechanism: make the other arm a dying local instead of a live
// one and use-after-scope fires for THAT arm, proving the conditional does propagate loans
// -- only the temporary arm contributed nothing.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile int cond = 0;
volatile char sink;

int main() {
  std::string live = "a-live-string-long-enough-that-its-buffer-is-heap-allocated";
  std::string_view v;
  {
    // The temporary in the second arm is extended to `r`'s scope and dies at the closing
    // brace. `live`'s loan in the first arm is what hid it.
    const std::string &r =
        cond ? live
             : (const std::string &)std::string(
                   "a-dead-string-long-enough-that-its-buffer-is-heap-allocated");
    v = r;
  }
  sink = v[0];
  return 0;
}
