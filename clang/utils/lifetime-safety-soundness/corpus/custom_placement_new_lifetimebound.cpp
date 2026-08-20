// DESC: a class-specific placement `operator new` returning a `char*` buffer it was
// handed. Whether an allocation function returns storage it was GIVEN is a property
// of its body, and was being guessed from its signature -- "the parameter after the
// size is `void*`, so this is the non-allocating form". That guess is wrong both
// ways: this one takes a `char*`, so it was treated as a fresh allocation and
// freeing the buffer left the placed object's borrow dangling with nothing
// reported; conversely a class-specific `operator new(size_t, void*)` that
// genuinely allocates (an allocator wrapper) was reported as a use-after-free that
// ASan says does not exist.
//
// '[[clang::lifetimebound]]' states exactly the relationship placement needs -- the
// result may point into this parameter -- so a user-written allocation function is
// now read from its annotation, and the existing lifetimebound body verifier keeps
// that annotation honest. An UNANNOTATED pointer placement parameter leaves the
// question open and is refused rather than assumed fresh. The reserved global
// `::operator new(size_t, void*)` is still recognized by signature, which is not a
// guess: [new.delete.placement] specifies it to return its second argument.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
volatile int sink;

struct Tag {};

struct T {
  int x;
  static void *operator new(unsigned long, char *p [[clang::lifetimebound]],
                            Tag) {
    return p;
  }
  static void operator delete(void *p [[clang::noescape]],
                              char *b [[clang::noescape]], Tag) noexcept {}
};

int main() {
  char *heap = new char[64];
  T *t = new (heap, Tag{}) T{7};
  delete[] heap;
  sink = t->x; // heap-use-after-free
  return 0;
}
