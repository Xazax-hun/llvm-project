// DESC: a RAW pointer (const char*) borrows into the buffer of an owner that is
// a member of a non-owner WRAPPER global, then the owner is reallocated from
// another function. The view-on-mutable-global check detects the wrapper via
// recordContainsMutableOwner, but the round-63 fix flagged only gsl::Pointer
// VIEW value types there, deliberately skipping raw pointers; a raw pointer into
// the buffer (`g_wrap.owner.data()`) therefore slipped. Found by the 64th
// multi-agent bypass hunt (B). Closed by also flagging a raw pointer/reference
// whose pointee is NOT itself an owner (i.e. it points into a buffer, not at a
// stable owner sub-object).
// EXPECT-ASAN: heap-use-after-free
#include <string>

struct Wrap {
  std::string owner;
};
Wrap g_wrap;

__attribute__((noinline)) void mutate() {
  g_wrap.owner = std::string(64000, 'B'); // realloc, cross-function
}

__attribute__((noinline)) int single() {
  const char *p = g_wrap.owner.data(); // raw borrow into the wrapped owner
  mutate();                            // invalidates elsewhere
  return p[0];                         // heap-use-after-free
}

int main() {
  g_wrap.owner = std::string(64, 'A');
  return single();
}
