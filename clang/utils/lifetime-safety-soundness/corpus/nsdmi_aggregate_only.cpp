// DESC: a self-referential class that is only ever AGGREGATE-initialized. A default
// member initializer is code owned by a declaration; it is normally carried by
// whichever constructor runs it and analyzed there, but an aggregate has no
// constructor at all, so the initializer appeared in no CFG anywhere and the hazard
// written in it was invisible. Adding `A() = default;` to the very same class made it
// report -- which is what places this on the construction path rather than on the
// class.
//
// Default member initializers are now analyzed on their own, the way namespace-scope
// initializers and default arguments already were. That also covers a temporary bound
// in one, which a per-class self-referential check alone would not have.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink;

struct [[gsl::Pointer(char)]] Cache {
  std::string text;
  // Binds to a sibling member: appending to `text` reallocates and `first` dangles.
  std::string_view first = text;
};

int main() {
  Cache c{"a heap string long enough to never be SSO at all here"};
  // Mutating the owner the view is bound to, through the same object.
  c.text.append("more text, forcing a reallocation of the buffer");
  g_sink = c.first[0];
  return 0;
}
