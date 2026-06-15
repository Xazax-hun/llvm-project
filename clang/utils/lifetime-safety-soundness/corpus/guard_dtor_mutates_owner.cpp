// DESC: an RAII guard that is a [[gsl::Pointer]] capturing a mutable owner via a
// lifetime_capture_by(this) constructor frees/reallocates that owner in its
// out-of-line destructor (`~Trigger() { o->grow(); }`). The construction was
// not treated as capturing a mutable alias and the destructor body is invisible
// to the intra-procedural analysis, so a string_view into the owner that
// outlived the guard's scope was a silent heap-use-after-free. The guard's
// destruction is now an assumed invalidation of the borrows it carries on the
// captured owner.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct [[gsl::Owner(char)]] MyOwner {
  std::string buf;
  MyOwner() : buf(100, 'a') {}
  std::string_view view() const [[clang::lifetimebound]] { return buf; }
  void grow() { buf.assign(100000, 'z'); } // reallocates buf's heap buffer
};

struct [[gsl::Pointer]] Trigger {
  MyOwner *o;
  Trigger(MyOwner *oo [[clang::lifetime_capture_by(this)]]) : o(oo) {}
  ~Trigger() { o->grow(); } // out-of-line: frees the buffer `v` borrows
};

volatile char g;

int main() {
  MyOwner owner;
  std::string_view v = owner.view(); // v borrows owner.buf's heap buffer
  { Trigger t(&owner); }             // ~Trigger -> owner.grow() frees it
  g = v.empty() ? 0 : v[0];          // heap-use-after-free
  return 0;
}
