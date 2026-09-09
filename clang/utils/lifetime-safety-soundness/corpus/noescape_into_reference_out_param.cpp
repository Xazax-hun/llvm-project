// DESC: a [[clang::noescape]] argument parked in a caller-owned object by a
// WHOLE-OBJECT assignment through a REFERENCE parameter. Such an assignment writes
// the caller's object exactly as `*ptr = ...` does, but its destination resolves
// statically, so only a flow was emitted and no store fact existed for the
// store-site checks to see -- the pointer spelling goes through the routed path and
// does emit one.
//
// So `*dst = Box::wrap(s)` was reported and `dst = Box::wrap(s)` was not, with the
// same annotations and the same escape. Storing into a MEMBER of the reference was
// reported too, which places this on the whole-object store rather than on
// references in general.
//
// The destination is a [[gsl::Owner]] because that is a single level of indirection:
// a `[[gsl::Pointer]]` out-parameter is refused by multilevel-indirection instead,
// so the escape would never have to be caught on its own.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char g_sink;

struct [[gsl::Owner(char)]] Box {
  static Box wrap(char *s [[clang::lifetimebound]]) {
    Box b;
    b.p = s;
    return b;
  }
  friend void stash(Box &dst [[clang::noescape]], char *s [[clang::noescape]]);
  friend int main();

private:
  char *p = nullptr;
};

void stash(Box &dst [[clang::noescape]], char *s [[clang::noescape]]) {
  dst = Box::wrap(s); // `s` is parked in the caller-owned `dst`
}

int main() {
  Box b;
  {
    std::string tmp("a heap string long enough to never be SSO at all here");
    stash(b, tmp.data()); // the caller obeys noescape and drops `tmp`
  }
  g_sink = *b.p;
  return 0;
}
