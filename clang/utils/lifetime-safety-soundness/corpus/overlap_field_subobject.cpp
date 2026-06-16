// DESC: a method that mutates its own owner field (`buf.push_back`) and takes a
// view PARAMETER that aliases that field -- called as `process(buf)` -- was a
// silent heap-use-after-free. The view param is not live after the call so the
// liveness-based invalidation pass missed it, and the argument-overlap check
// compared loan access paths by EQUALITY, which didn't relate the view's
// field-rooted loan (this->buf) to the mutated receiver `this`. The overlap
// check now uses storage CONTAINMENT (a borrow into a subobject of the mutated
// owner aliases it), so the receiver + a field-borrow co-argument overlap.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
volatile char sink;
struct S {
  std::string buf = "hello world hello world xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
  __attribute__((noinline)) void process(std::string_view v [[clang::noescape]]) {
    for (int i = 0; i < 100000; i++)
      buf.push_back('z');               // reallocates this->buf's heap buffer
    sink = v.empty() ? 0 : v[0];        // v aliased buf -> dangling read
  }
  void run() { process(buf); }          // passes a view aliasing this->buf
};
int main() {
  S s;
  s.run();
  return 0;
}
