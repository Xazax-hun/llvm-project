// DESC: an escape into a caller-owned object is written through `(&c)->field` instead of
// `c.field`. Storing a borrow of a callee local into an object the caller owns is reported at
// the store, and it was -- for `c.current = &v`, for `(*&c).current = &v`, and for
// `Cache *lp = &c; lp->current = &v`. Only the `(&c)->current` spelling was silent, with the
// same source, the same destination and the same annotations.
//
// For an ARROW store the container is what the base points AT, so the check descends from the
// base's origin to its pointee: a pointer VARIABLE has storage of its own, and the object it
// designates lives on the pointee origin. That descent was applied to every arrow base except
// `this`, which was excluded by name.
//
// But `this` is not special -- it is a PRVALUE. A prvalue pointer expression has no storage,
// so its own origin already denotes what it points at, and descending goes a level too deep
// and loses the object entirely. `&c` is exactly such an expression, and so is a call
// returning a pointer. Asking the base's value category covers `this` and every other prvalue
// receiver at once, instead of enumerating spellings.
//
// The object starts out holding an immortal borrow so the lost-borrow sentinel has nothing to
// say; without that the analysis refuses the function rather than diagnosing it.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

static const int kInit = 0;

class [[gsl::Owner(int)]] Cache {
  const int *current = nullptr;
public:
  explicit Cache(const int *init [[clang::lifetimebound]]) : current(init) {}
  int read() const { return *current; }

  // Points the cache at a scratch buffer that dies when this function returns.
  friend void refresh(Cache &c [[clang::noescape]]) {
    std::vector<int> scratch(64, 7);
    (&c)->current = &scratch[0];
  }
};

int main() {
  Cache c(&kInit);
  refresh(c);        // `scratch` is freed on return
  sink = c.read();   // reads the freed buffer
  return 0;
}
