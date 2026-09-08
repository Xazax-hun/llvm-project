// DESC: a reseat of a BASE SUBOBJECT silencing a dangle in a member the derived class
// adds. Assigning to a base reaches the base and nothing else, but a [[gsl::Pointer]]
// object is a single origin, so the base subobject and the whole object ARE that one
// origin -- and the casts naming the base are stripped before the store is modelled
// (the derived-to-base reference cast by the value-preserving cast loop, the implicit
// one by IgnoreParenImpCasts). What arrived looked like a whole-object store, so it
// killed the object's loans and its liveness, discarding a borrow it never touched.
//
// Both spellings of the same call were affected -- `static_cast<Base &>(d) = ...` and
// `d.Base::operator=(...)` -- and removing the base assignment made the identical
// dangle report, which is what pins it on the store rather than on the borrow.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink;

static const char kImmortal[] = "immortal storage, long enough to never be SSO";

struct [[gsl::Pointer(char)]] Base {
  std::string_view b;
  Base() = default;
  explicit Base(std::string_view s [[clang::lifetimebound]]) : b(s) {}
};

struct [[gsl::Pointer(char)]] Derived : Base {
  // The member the base assignment does not touch.
  std::string_view v;
  Derived() = default;
  void use() const { g_sink = v[0]; }
};

int main() {
  Derived d;
  d.v = std::string_view(kImmortal);
  {
    std::string tmp("a heap string long enough to never be SSO at all here");
    d.v = tmp;                                        // d.v borrows tmp
    static_cast<Base &>(d) = Base(std::string_view(kImmortal)); // reseats the BASE only
  }
  d.use(); // reads d.v, a borrow of the freed string
  return 0;
}
