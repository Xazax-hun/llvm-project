// DESC: a function takes two std::string by non-const reference; a view is taken
// from one parameter and the other is mutated. Called with the SAME object for
// both parameters, the two references alias, so mutating one reallocates the
// buffer the view borrows. The aliasing is invisible inside the callee (distinct
// reference roots); it must be caught at the call site (`worker(s, s)`).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

static char read(std::string_view v [[clang::noescape]]) { return v.size() ? v[0] : 0; }

static void worker(std::string &a [[clang::noescape]],
                   std::string &b [[clang::noescape]]) {
  std::string_view v = a;   // borrows a's buffer
  b.append(20000, 'b');     // reallocates b (== a) -> v dangles
  (void)(read(v) == 'a');
}

int main() {
  std::string s(64, 'a');
  worker(s, s);             // the two reference parameters alias
  return 0;
}
