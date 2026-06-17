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
