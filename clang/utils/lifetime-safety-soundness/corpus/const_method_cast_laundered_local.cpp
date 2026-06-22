// DESC: a const member function casts away constness via a C-style void* round-
// trip BOUND TO A LOCAL POINTER, then mutates the owner through it -- reallocating
// a buffer a caller's lifetimebound view still borrows. The const-subversion
// analysis walks only the mutating call's own receiver back to `this`, so a cast
// laundered through a local (`Box* m = (Box*)(const void*)this; m->s.resize(...)`)
// escaped it; the void* roundtrip also dodged the const_cast-keyword check. Found
// by the multi-agent bypass hunt. Fixed by warning at the CAST SITE on any
// C-style/functional cast that casts away constness (like the const_cast keyword),
// independent of how the result is later used.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct Box {
  std::string s{"a fairly long heap string xxxxxxxxxxxxxxxxxxxxxxx"};
  std::string_view view() const [[clang::lifetimebound]] { return s; }
  void grow() const {
    Box *m = (Box *)(const void *)this; // casts away const, laundered via a local
    m->s.resize(500000, 'x');           // realloc -> old buffer freed
  }
};

int main() {
  Box b;
  std::string_view v = b.view(); // borrow into b.s (tracked, lifetimebound)
  b.grow();                      // const method reallocates b.s -> v dangles
  int acc = 0;
  for (char c : v) // use-after-free read
    acc += c;
  return acc & 1;
}
