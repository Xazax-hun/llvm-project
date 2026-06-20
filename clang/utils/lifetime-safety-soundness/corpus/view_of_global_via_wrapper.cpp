// DESC: a view of a mutable global is laundered through a non-owner WRAPPER. The
// global g_wrap is a plain struct (NOT a gsl::Owner) that CONTAINS a
// std::string; a [[lifetimebound]] accessor returns a string_view of that
// member. The borrow's loan anchors at the whole g_wrap object, whose type is
// not a gsl::Owner, so the view-on-mutable-global check (which keyed on the loan
// root VarDecl being a gsl::Owner) missed it; the view is then cached into a
// [[gsl::Owner]]'s opaque member, so no use fires, and the dangling read is in
// another function. Found by the 63rd multi-agent bypass hunt (A). Closed by
// detecting a global whose type transitively contains a mutable owner
// (recordContainsMutableOwner), flagging a gsl::Pointer view that borrows into
// it.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct Wrapper {
  std::string owner{std::string(100, 'A')};
  std::string_view get() const [[clang::lifetimebound]] { return owner; }
};
Wrapper g_wrap; // global non-owner struct CONTAINING a mutable owner

struct [[gsl::Owner]] Cache {
private:
  std::string_view sv;

public:
  void refresh() { sv = g_wrap.get(); } // borrow rooted at g_wrap (non-owner)
  char first() const { return sv.empty() ? '?' : sv[0]; }
};

__attribute__((noinline)) int bug() {
  Cache c;
  c.refresh();
  g_wrap.owner = std::string(100, 'B'); // realloc -> sv dangles
  return c.first();                     // heap-use-after-free
}

int main() { return bug(); }
