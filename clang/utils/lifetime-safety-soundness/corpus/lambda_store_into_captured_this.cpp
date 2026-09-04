// DESC: the store into the object happens inside a LAMBDA that captures `this`. A lambda body
// is a separate function: nothing it writes is flowed back to the enclosing function (the
// closure gets a single merged origin, which tracks only whether the lambda outlives a
// capture), and the body's own analysis cannot see the lifetimes of the enclosing locals it
// captured -- from inside, they are just variables belonging to some other function.
//
// So the borrow is invisible from both sides. The enclosing function sees a closure being made
// and called; the lambda sees a store of a borrow whose owner it has no reason to doubt. The
// identical store written directly in the method body is reported.
//
// This is the same "not flowed back" gap that a by-reference capture of an indirection-typed
// variable is already refused for, reached through the captured OBJECT instead of through the
// captured variable. It is refused rather than modeled: making the enclosing function see into
// the body would be inter-procedural, which this analysis is not.
//
// The object starts out holding an immortal borrow so the lost-borrow sentinel has nothing to
// say; without that the analysis refuses the function for that reason instead.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

static const char kInit[] = "immortal";

struct [[gsl::Pointer(char)]] Holder {
  std::string_view v;

  explicit Holder(std::string_view i [[clang::lifetimebound]]) : v(i) {}

  // Reads as ordinary code: a small helper closure that updates a member.
  void go() {
    std::string local(4096, 'a');
    [this, &local] { v = std::string_view(local); }();
  } // `local` is freed here, and `v` still points into its buffer
};

int main() {
  Holder h(kInit);
  h.go();
  sink = h.v[0];
  return 0;
}
