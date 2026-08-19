// DESC: a scope guard written as the operand of a comma operator. Destroying a
// guard that holds a borrow is modelled as an assumed invalidation, and the
// handler for the CFGTemporaryDtor form has to tell a DISCARDED temporary (its
// own to model) from one CONSUMED as a subexpression (modelled where it is
// consumed). That test bailed on any enclosing expression -- effectively a
// blocklist of the contexts that discard a value -- and the comma operator was
// missing from it. A built-in comma is not a call, so it consumes nothing:
// `(Guard{&s}, 0)` discards the guard exactly as `Guard{&s};` does, which was
// already reported. An overloaded operator taking the guard as a temporary
// argument IS a call, so those were reported all along; the built-in comma is
// the one operator that is not.
// FLAGS: -Wno-unused-value
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Pointer]] Guard {
  std::string *p;
  ~Guard() { p->reserve(8192); } // reallocates -> frees the old buffer
};

int main() {
  std::string s = "a long heap allocated string value that is definitely not SSO!!";
  std::string_view v = s;
  (Guard{&s}, 0); // temporary dies here, reallocating s
  sink = v[0];    // heap-use-after-free
  return 0;
}
