// DESC: a LOCAL's borrow stored into a private member of a [[gsl::Owner]] parameter. The
// noescape check only fires when the SOURCE is an annotated parameter, so `c.d = s` with a
// noescape `s` was reported while `c.d = l.c_str()` with a local `l` was not -- and nothing
// else covered it. Expiry cannot see it: an owner's members are opaque, so the private `d` has
// no origin of its own and the borrow lands on a transient expression origin, leaving no live
// origin holding it at the local's expiry. The multi-level-indirection refusal that rejects a
// pointer-like out-parameter -- and which does cover the [[gsl::Pointer]] spelling of this
// same shape -- does not apply, because an owner is a single level of indirection. Every
// annotation here is TRUTHFUL: neither `c` nor anything else escapes the function; the local
// simply does not outlive the caller's object.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct [[gsl::Owner(char)]] Box {
  friend void f2(Box &c [[clang::noescape]]);
  char peek() const { return d[0]; }
private:
  const char *d = "";
};

void f2(Box &c [[clang::noescape]]) { std::string l(64, 'x'); c.d = l.c_str(); }

int main() {
  Box b;
  f2(b);
  sink = b.peek();
  return 0;
}
