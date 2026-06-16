// DESC: a [[gsl::Pointer]] wrapper that points AT an owner (`std::string* p`,
// bound through a lifetime_capture_by(this) constructor) has a non-const method
// that mutates the pointee owner while a co-argument view aliases it -- a silent
// heap-use-after-free. The borrow into the owner lives on the wrapper's POINTEE
// origin (one indirection level in), not on the wrapper's own origin, so the
// argument-overlap check missed it: it only read the mutating receiver's
// top-level loans. The view param is not live after the call so the
// liveness-based invalidation pass missed it too. The overlap check now unions
// the loans across a gsl::Pointer receiver's pointee chain.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
volatile char sink;
struct [[gsl::Pointer]] W {
  std::string *p;
  W(std::string &s [[clang::lifetime_capture_by(this)]]) : p(&s) {}
  __attribute__((noinline)) void grow_and_use(std::string_view v
                                               [[clang::noescape]]) {
    for (int i = 0; i < 100000; i++)
      p->push_back('x');             // reallocates *p's heap buffer
    sink = v.empty() ? 0 : v[0];     // v aliased *p -> dangling read
  }
};
int main() {
  std::string s = "hello world hello world xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
  W w(s);
  std::string_view v = s;            // view aliases s == *w.p
  w.grow_and_use(v);
  return 0;
}
