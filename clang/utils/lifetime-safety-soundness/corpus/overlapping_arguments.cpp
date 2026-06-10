// DESC: a function is passed an owner by mutable reference AND a string_view
// that borrows it. The callee reallocates the owner and then reads the now-
// dangling view. No lifetime annotation expresses that two arguments must not
// alias, so the overlap is invisible at the (validly annotated) signature; the
// caller must avoid passing overlapping borrows.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

void f(std::string &s [[clang::noescape]], std::string_view v [[clang::noescape]]) {
  s.append(10000, 'b');                   // reallocates s -> v dangles
  volatile char c = v.size() ? v[0] : 0;  // use-after-free (v aliased s)
  (void)c;
}

int main() {
  std::string s = std::string(50, 'a');
  std::string_view v = s; // v borrows s
  f(s, v);                // passes mutable s AND a view aliasing it
  return 0;
}
