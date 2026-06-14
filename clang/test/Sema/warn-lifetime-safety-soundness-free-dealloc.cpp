// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-invalidation -verify %s

// A C-style / explicit-call deallocation -- free(p), realloc(p, n), or a direct
// ::operator delete(p) call -- frees its first argument, just like a `delete`
// expression. The analysis models these as deallocations, so a borrow into the
// freed storage that is used afterward is reported as an invalidation. (A
// `delete` *expression* is handled separately; this covers the call forms.)

extern "C" __attribute__((malloc)) void *malloc(unsigned long) noexcept;
extern "C" void free(void *) noexcept;
extern "C" void *realloc(void *, unsigned long) noexcept;

void use(int *);

void free_invalidates_alias() {
  int *p = (int *)malloc(16); // expected-warning {{object whose reference is captured is later invalidated}}
  int *alias = p;
  free(p); // expected-note {{invalidated here}}
  use(alias); // expected-note {{later used here}}
}

void realloc_invalidates_old() {
  int *p = (int *)malloc(16); // expected-warning {{object whose reference is captured is later invalidated}}
  int *alias = p;
  p = (int *)realloc(p, 64); // expected-note {{invalidated here}}
  use(alias);                // expected-note {{later used here}}
  use(p);
}

void operator_delete_call_invalidates() {
  int *p = new int(7); // expected-warning {{object whose reference is captured is later invalidated}}
  int *alias = p;
  ::operator delete(p); // expected-note {{invalidated here}}
  use(alias); // expected-note {{later used here}}
}

// Negative: freeing a pointer no borrow outlives is fine (no live alias).
void free_no_live_borrow() {
  int *p = (int *)malloc(16);
  free(p); // no-warning
}
