// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A non-allocating placement-new returns the storage it is given, so a borrow
// into the placed object dangles when that storage is freed. The analysis
// models this by forwarding the placement buffer's loan to the result -- but it
// only did so for the single-placement-argument form. A placement operator with
// EXTRA placement arguments (a tag, allocator state, ...) -- e.g.
// `new (buf, Tag{}) T` -- fell through to being issued a FRESH heap-allocation
// loan decoupled from the buffer, so freeing the buffer did not dangle the
// borrow: a silent heap-use-after-free. The buffer's loan is now forwarded for
// any non-allocating placement form (first placement parameter is `void*`),
// regardless of how many placement arguments follow.

namespace std {
using size_t = decltype(sizeof(0));
}
void *operator new(std::size_t, void *p) noexcept; // single-arg placement

struct Tag {};
void *operator new(std::size_t, void *p [[clang::lifetimebound]], Tag) noexcept {
  return p;
}

struct T {
  int v;
};

int multi_arg_placement() {
  char *buf = new char[sizeof(T)]; // expected-warning {{allocated object does not live long enough}}
  T *p = new (buf, Tag{}) T{42};   // 2 placement args
  int *q = &p->v;
  delete[] buf;   // expected-note {{freed here}}
  return *q;      // expected-note {{later used here}}
}

// Control: the single-argument placement form was already modeled.
int single_arg_placement() {
  char *buf = new char[sizeof(T)]; // expected-warning {{allocated object does not live long enough}}
  T *p = new (buf) T{42};
  int *q = &p->v;
  delete[] buf;   // expected-note {{freed here}}
  return *q;      // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Whether an allocation function returns storage it was HANDED is a property of
// its body, and was being guessed from its signature: "the parameter after the
// size is `void*`, so this is the non-allocating form". That guess is wrong both
// ways, so it is replaced by the annotation, which states exactly the
// relationship placement needs -- the result may point into this parameter.
//===----------------------------------------------------------------------===//

// A custom allocation function returning a `char*` buffer it was handed IS
// placement. Its parameter is not `void*`, so the signature guess called it a
// fresh allocation and freeing the buffer left the placed object's borrow
// dangling with nothing reported.
struct Tagged {
  int x;
  static void *operator new(std::size_t, char *p [[clang::lifetimebound]], Tag) {
    return p;
  }
  // expected-warning@+1 2 {{parameter that can hold a borrow is not annotated}}
  static void operator delete(void *, char *, Tag) noexcept {}
};

int custom_placement_annotated() {
  char *buf = new char[64]; // expected-warning {{allocated object does not live long enough}}
  Tagged *t = new (buf, Tag{}) Tagged{7};
  delete[] buf;   // expected-note {{freed here}}
  return t->x;    // expected-note {{later used here}}
}

// The same function WITHOUT the annotation leaves the question open. Answering
// it "fresh" would silently drop the borrow, so the placement argument is
// refused like any argument bound to an unannotated borrow-holding parameter.
struct Unannotated {
  int x;
  // The body returns the parameter, so the existing lifetimebound machinery also
  // points out that the annotation is the one missing here.
  // expected-warning@+3 {{parameter that can hold a borrow is not annotated}}
  // expected-warning@+2 {{should be marked [[clang::lifetimebound]]}}
  // expected-note@+1 {{param returned here}}
  static void *operator new(std::size_t, char *p, Tag) { return p; }
  // expected-warning@+1 2 {{parameter that can hold a borrow is not annotated}}
  static void operator delete(void *, char *, Tag) noexcept {}
};

int custom_placement_unannotated() {
  char *buf = new char[64];
  // expected-warning@+1 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
  Unannotated *t = new (buf, Tag{}) Unannotated{7};
  delete[] buf;
  return t->x;
}

// The other direction: a class-specific allocation function that TAKES a `void*`
// and genuinely allocates is not placement. The signature guess called it
// placement and reported a use-after-free that does not exist.
struct Wrapper {
  int x;
  // expected-warning@+2 {{lifetime safety cannot track}}
  // expected-warning@+1 {{parameter that can hold a borrow is not annotated}}
  static void *operator new(std::size_t n, void *) { return ::operator new(n); }
  // expected-warning@+3 {{freeing a pointer whose allocation was not seen}}
  // expected-warning@+2 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
  // expected-warning@+1 2 {{parameter that can hold a borrow is not annotated}}
  static void operator delete(void *p, void *) noexcept { ::operator delete(p); }
};

int allocating_wrapper() {
  char *buf = new char[64];
  // expected-warning@+1 {{argument is bound to a parameter that can hold a borrow but is not annotated}}
  Wrapper *w = new (buf) Wrapper{7};
  delete[] buf;
  // no use-after-free: `w` does not point into `buf`.
  return w->x; // expected-warning {{lifetime safety cannot track local variable 'w'}}
}
