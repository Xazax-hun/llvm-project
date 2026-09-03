// DESC: a borrow of a BY-VALUE parameter's own storage escapes into an object the CALLER
// owns. `load(Cache &c [[noescape]], std::string v)` stores `v.c_str()` into a private cursor
// of `c`; the parameter object `v` is a copy that dies with the call, so the caller is left
// holding a pointer into a freed heap buffer. Both annotations are truthful -- `c` really does
// not escape -- and the store is the callee's own business, so nothing else had cause to
// complain.
//
// The store-site check that covers this asked "is the borrowed thing a parameter?" and
// excluded every parameter, on the grounds that a parameter's own storage escaping is the
// noescape question. True for a REFERENCE or POINTER parameter, which denotes storage the
// caller owns; false for a by-value one, whose parameter object dies with the call exactly as
// a local does. Copying `v` into a local first WAS reported, so only the root of the borrow
// differed between the caught and the missed spelling.
//
// The object starts out holding an immortal borrow so the lost-borrow sentinel has nothing to
// say; without that the analysis refuses the function instead of diagnosing it, which is sound
// but says nothing about this store.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

static const char kInit[] = "immortal";

class [[gsl::Owner(char)]] Cache {
  const char *d_ = ""; // private, non-owning cursor
public:
  explicit Cache(const char *init [[clang::lifetimebound]]) : d_(init) {}
  char read() const { return d_[0]; }

  // Caches a view of the key it is handed. Reads as a sensible helper: `c` does not escape,
  // and `v` is the callee's own copy to do with as it likes -- except point into it.
  friend void load(Cache &c [[clang::noescape]], std::string v) { c.d_ = v.c_str(); }
};

int main() {
  Cache c(kInit);
  load(c, std::string(4096, 'q')); // the parameter object dies at the end of this statement
  sink = c.read();
  return 0;
}
