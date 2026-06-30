// DESC: a [[gsl::Owner]] DERIVED class whose borrow-capture machinery lives on a
// non-gsl BASE: the base has a lifetime_capture_by(this) setter and a public
// borrow-holding member. A gsl::Owner is trusted as opaque (suppressing
// unknown-ownership), but the owner-encapsulation checks (owner-capture,
// owner-public-pointer) only inspected the owner's OWN members, not those
// inherited from the base -- so the lying owner slipped through, and the scalar
// peek() accessor avoided the use-site unannotated-indirection net. The checks
// now traverse inherited (non-owner) base members.
// EXPECT-ASAN: heap-use-after-free
#include <string>

struct Base {
  const char *hidden = nullptr;
  void stash(const char *p [[clang::lifetime_capture_by(this)]]) { hidden = p; }
  char peek() const { return hidden ? *hidden : 0; } // scalar read of the borrow
};

struct [[gsl::Owner(char)]] Liar : Base {};

volatile char sink;

int main() {
  Liar L;
  {
    std::string s = "a sufficiently long heap allocated string for the capture";
    L.stash(s.c_str()); // captures a borrow into L.hidden (inherited)
  }                     // s dies; L.hidden dangles
  sink = L.peek();      // use-after-free read
  return sink;
}
