// DESC: a borrow obtained from a [[clang::lifetimebound]] accessor called on
// `this` (`v = this->get()`) is laundered into the coarse `$this` placeholder
// loan, which carries no issuing expression and is not a placeholder parameter.
// The checker had no reportable anchor for such a loan and silently skipped it
// during invalidation checking, so a later self-mutation of the borrowed owner
// (`this->grow()`, reallocating buf) went unreported -- a silent
// heap-use-after-free. The borrow is now anchored at the use that keeps it live
// (a precise field loan, when present, is still preferred so direct borrows
// anchor at the borrow site).
// EXPECT-ASAN: heap-use-after-free
#include <cstdio>
#include <string>
#include <string_view>
struct W {
  std::string buf = "a long heap string exceeding sso limits..........";
  const char *get() const [[clang::lifetimebound]] { return buf.data(); }
  void grow() { buf.append(2000, 'z'); } // reallocates buf
  __attribute__((noinline)) int bug() {
    const char *v = this->get(); // borrow laundered through `$this`
    this->grow();                // invalidates v
    return v[0];                 // reads freed heap
  }
};
int main() {
  W w;
  printf("%d\n", w.bug());
  return 0;
}
