// DESC: a [[gsl::Owner]] with a private std::string_view member caches a view of
// a mutable global std::string. The borrow enters via a global read (no
// parameter crosses, so unannotated-indirection never applies) and lands in the
// owner's opaque member, where the loan-based view-on-mutable-global pass cannot
// see it (an owner's contents are not tracked) -- and the dangling read is in a
// different function from the store, so intra-procedural tracking cannot connect
// them. The view-on-mutable-global check is therefore moved to the view's
// CREATION site (the owner->view conversion `cache = g_owner`), independent of
// where the view is stored. Found by the 62nd multi-agent bypass hunt (D).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

std::string g_owner;

struct [[gsl::Owner]] Container {
private:
  std::string_view cache;

public:
  void refresh() { cache = g_owner; } // view of the mutable global cached
  char first() const { return cache.empty() ? '?' : cache[0]; }
};

__attribute__((noinline)) int bug() {
  Container c;
  c.refresh();                 // c.cache now views g_owner's heap buffer
  g_owner.assign(100000, 'B'); // realloc frees the old buffer -> cache dangles
  return c.first();            // heap-use-after-free read
}

int main() {
  g_owner.assign(64, 'A');
  return bug();
}
