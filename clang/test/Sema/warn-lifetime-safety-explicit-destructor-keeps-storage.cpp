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
// The object IS dead afterwards, so a use that FOLLOWS the pointer must still be
// reported. The storage surviving makes the pointer a valid pointer; it does not
// resurrect the object.
//===----------------------------------------------------------------------===//

void destroy_twice(S *p) { // expected-warning {{is later invalidated}}
  p->~S();                 // expected-note {{invalidated here}}
  p->~S();                 // expected-note {{later used here}}
}

void destroy_then_read(S *p) { // expected-warning {{is later invalidated}}
  p->~S();                     // expected-note {{invalidated here}}
  (void)p->id;                 // expected-note {{later used here}}
}

void destroy_then_deref(S *p) { // expected-warning {{is later invalidated}}
  p->~S();                      // expected-note {{invalidated here}}
  S copy = *p;                  // expected-note {{later used here}}
  (void)copy;
}

// A mere content mutation leaves the object alive, so a pointer at it is fine even
// when dereferenced -- this is what the exemption is for, and the destruction rule
// above must not disturb it.
void mutate_twice_through_pointer(S *p) {
  p->id = 1;
  p->id = 2;
}

//===----------------------------------------------------------------------===//
// KNOWN GAP: element identity. `p[0].~S(); p[1].~S();` still reports, because the
// model gives every element of `p` the same loan, so the second subscript is a
// dereference of what the model considers the same, now-destroyed object.
// Distinguishing elements is out of scope here.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Whether the STORAGE was also released makes no difference to this rule.
//
// `destructsFirstArg` covers more than the explicit destructor call: `free`,
// `realloc`, a direct `operator delete` call and an `ownership_takes(this)`
// release method all end the object's lifetime AND give the storage back. But
// `++p` is equally harmless either way, and a dereference equally wrong, so the
// two need not be told apart -- the rule is just "lifetime ended + use follows the
// pointer".
//
// Telling them apart is what made `free(p); ++p;` and `delete p; ++p;` report,
// which is noise: a non-dereferencing use of a freed pointer is not a temporal
// memory error, and out-of-bounds is out of scope for this analysis.
//===----------------------------------------------------------------------===//

extern "C" void free(void *);
void operator delete(void *) noexcept;

// Following the pointer after the storage is gone: reported.
void freed_then_dereferenced(S *p) { // expected-warning {{is later invalidated}}
  free(p);                           // expected-note {{invalidated here}}
  (void)p->id;                       // expected-note {{later used here}}
}

// Only reading the pointer value: not reported, exactly as after a destructor
// call. These three are the same rule, not three rules.
void freed_then_advanced(S *p) {
  free(p);
  ++p;
}

void operator_deleted_then_advanced(S *p) {
  ::operator delete(p);
  ++p;
}

void deleted_then_advanced(S *p) {
  delete p;
  ++p;
}
