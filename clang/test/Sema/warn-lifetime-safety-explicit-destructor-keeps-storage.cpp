// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety -verify %s

// An explicit destructor call ends the object's lifetime but LEAVES THE STORAGE
// -- that is the point of one, since placement-new may reuse it. So a pointer or
// reference AT the object is still a valid pointer afterwards, and the "an object
// survives a mutation of its own contents" exemption applies to it just as it does
// to `v.push_back()`.
//
// It did not, because the exemption was cancelled for anything flagged a
// "deallocation", and an explicit destructor call sets that flag alongside
// `delete`. The flag was answering two different questions with one bit: "does the
// object's lifetime end" (true for both, and what the naked-delete and
// ownership_takes(this) checks want) and "does the storage go away" (only
// `delete`, and the only thing that can make a pointer AT the object dangle).
//
// Note this was never pointer-specific: a reference receiver reported identically.

struct S {
  int id;
  ~S();
};

//===----------------------------------------------------------------------===//
// Destroying through a pointer or reference, then using the pointer VALUE.
//===----------------------------------------------------------------------===//

void destroy_loop(S *begin, S *end) {
  for (S *current = begin; current != end; ++current)
    current->~S();
}

void destroy_then_advance(S *p) {
  p->~S();
  ++p;
}

void destroy_through_reference(S &r, S *p) {
  r.~S();
  (void)p;
}

// A non-destroying mutation through either was already fine; kept so the two
// cannot drift apart again.
void mutate_then_use(S *p, S &r) {
  p->id = 1;
  ++p;
  r.id = 2;
}

//===----------------------------------------------------------------------===//
// `delete` DOES release the storage, so a pointer at the object dangles and must
// still be reported. This is the case the cancellation was introduced for.
//===----------------------------------------------------------------------===//

// `delete` also expires the allocation, so this lands in the use-after-free path
// rather than the invalidation one -- either way it must not be lost.
void delete_then_use(S *q) { // expected-warning {{parameter does not live long enough}}
  delete q;                  // expected-note {{freed here}}
  (void)q->id;               // expected-note {{later used here}}
}

void delete_address_of_local() {
  S obj;
  S *p = &obj; // expected-warning {{allocated object does not live long enough}}
  delete &obj; // expected-note {{freed here}}
  (void)p->id; // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// KNOWN GAP, deliberately not covered here: destroying the SAME object twice
//
//   void destroy_twice(S *p) { p->~S(); p->~S(); }
//
// used to be reported and no longer is. The model gives `p`, `p + 1` and
// `current` on successive iterations the SAME loan -- pointer arithmetic
// deliberately preserves it -- so "destroy the storage named by $p twice" is the
// same fact in `destroy_twice` and in `destroy_loop` above, which destroys a
// different element each time. Separating them needs element identity, which the
// model does not have; reporting is what produced the loop false positive. A
// double destruction is also not an aliasing question, so it belongs in a check
// of its own rather than in this one.
//===----------------------------------------------------------------------===//
