// DESC: a method publishes a borrow held by one of its object's MEMBERS to a global. The object
// is a local view holding a borrow of a local string; the method copies that member into a
// global, and after the enclosing scope ends the global points into freed memory.
//
// The rule exists -- a borrow of the enclosing object or one of its members must not escape to
// global storage, because intra-procedurally the object is caller-scope and never expires -- and
// the escape fact was emitted. What failed was classifying the loan: a member's origin is seeded
// at entry with an Uninitialized loan naming that field, and its path kind is Uninitialized
// rather than ValueDecl, so getAsValueDecl() returned null and the escape counted as neither a
// `this` borrow nor a field borrow.
//
// So publishing a MEMBER's borrow was silent while publishing a LOCAL's borrow one function over
// was reported. Not about constness: the non-const spelling and a whole-object store were
// equally silent.
//
// The global is seeded with an immortal borrow first, so the lost-borrow sentinel has nothing to
// say; without that the analysis refuses the read rather than diagnosing the escape.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
volatile char sink;
static const char G[] = "hello";
std::string_view g = std::string_view(G);

struct [[gsl::Pointer]] L {
  std::string_view q;
  void go() const { g = q; }   // store a borrow HELD BY a member of `this` into a global
};

void run() {
  std::string s(64, 'x');
  L l{s};
  l.go();
}

int main() {
  g = std::string_view(G);     // seed a valid loan (masks the lost-loan sentinel)
  run();
  sink = g[0];                 // heap-use-after-free
}
