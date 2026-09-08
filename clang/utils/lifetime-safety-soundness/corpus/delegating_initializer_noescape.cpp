// DESC: a [[clang::noescape]] parameter forwarded to a DELEGATING constructor that
// stores it. A delegating initializer (`: E(init)`) initializes the object being
// constructed -- more directly than a base initializer, which reaches only a
// subobject -- but only the base spelling was modelled, so a delegating one fell
// through and modelled nothing.
//
// The parameter therefore came to rest in the object with nothing said, while the
// same parameter stored directly by the constructor was reported. Every annotation
// here is truthful except that one: `Box::set` is honest per `E`'s declaration, which
// is what makes the caller's obedience to noescape fatal -- it drops the borrowed
// string, and the object keeps pointing into it.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

using sv = std::string_view;
volatile char g_sink;

struct [[gsl::Pointer(char)]] E {
  sv v;
  E(sv s [[clang::lifetimebound]], int) : v(s) {}
  // The lie: `s` comes to rest in the object, via the delegated-to constructor.
  E(sv s [[clang::noescape]], char) : E(s, 0) {}
  void use() const { g_sink = v[0]; }
};

struct [[gsl::Pointer(char)]] Box {
  E e;
  Box(sv s [[clang::lifetimebound]]) : e(s, 0) {}
  void set(sv s [[clang::noescape]]) { e = E(s, 'c'); } // honest per E's declaration
  void use() const { e.use(); }
};

// Gives `b` a real borrow at construction, so the lost-borrow sentinel does not fire
// and the bypass is clean.
const std::string keep_alive("a long lived heap string, definitely not SSO at all");

int main() {
  Box b{keep_alive};
  {
    std::string tmp("a heap string long enough to not be SSO at all here");
    b.set(tmp); // caller obeys noescape and drops `tmp`
  }
  b.use(); // reads a borrow of the dead string
  return 0;
}
