// DESC: the store into the object is written through an ALIAS of it rather than by naming the
// member on `this`. `Box &s = *this; s.d = local.c_str();` was silent, while the identical
// `d = local.c_str();` one line away was reported -- and so were four other ways of naming the
// same object: a pointer to it, a dereference of that pointer, a conditional between two names
// for it, and a [[clang::lifetimebound]] accessor returning it.
//
// CAUSE: binding an alias hands it a COPY of the object's origin tree. Loans flow in at the
// binding and nothing flows back out, so a store through the alias deposits the borrow in the
// copy while every check reads the object's own origin. A back-flow cannot fix it either: loan
// propagation is a forward dataflow, so an edge at the binding carries nothing -- the borrow
// does not exist yet at that program point.
//
// FIX: route the store by the loans its own member LVALUE holds. Those name the storage being
// written -- the object's loan projected by the field -- so they reach the object's field
// origin whatever expression designated the object. One rule for every spelling, where matching
// them in the AST would be an open-ended list.
//
// The object starts out holding an immortal borrow so the lost-borrow sentinel has nothing to
// say; without that the analysis refuses the function rather than diagnosing it.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

static const char kInit[] = "immortal";

class [[gsl::Owner(char)]] Box {
  const char *d = "";
public:
  explicit Box(const char *i [[clang::lifetimebound]]) : d(i) {}
  char read() const { return d[0]; }

  // Reads as ordinary code: a local alias for the object, then a member store.
  void set() {
    std::string local(4096, 'a');
    Box &self = *this;
    self.d = local.c_str();
  } // `local` is freed here, and `d` still points into its buffer
};

int main() {
  Box b(kInit);
  b.set();
  sink = b.read();
  return 0;
}
