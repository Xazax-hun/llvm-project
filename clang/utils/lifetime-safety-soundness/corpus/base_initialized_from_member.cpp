// DESC: a flush-on-destroy mixin: a base class initialized with a MEMBER of the
// derived object. A base subobject is a borrow-holding subobject of `this` exactly
// as a member is, but only the member spelling produced a FieldStore, so only it was
// recognised as self-referential -- the base spelling modelled the deposit and
// recorded no store, so the checker never looked.
//
// The base form is the worse of the two: a base is initialized BEFORE the members
// and destroyed AFTER them, so the borrow refers to a member that is not constructed
// yet, and the base's destructor reads it once it is gone. Nothing is moved or
// mutated -- it dangles unconditionally.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char g_sink;

struct [[gsl::Pointer]] Flusher {
  const std::string *target;
  Flusher(const std::string &t [[clang::lifetimebound]]) : target(&t) {}
  // Runs AFTER ~buffer, which has already freed the storage.
  ~Flusher() { g_sink = (*target)[0]; }
};

struct [[gsl::Pointer]] Session : Flusher {
  std::string buffer;
  // `Flusher(buffer)` runs before `buffer` is constructed.
  Session() : Flusher(buffer), buffer(50, 'x') {}
};

int main() {
  Session s;
  return 0;
}
