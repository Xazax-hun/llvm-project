// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety -verify %s

// Two questions about a dangling pointer, answered over ALL the uses forward of
// the point where its pointee died rather than over the nearest one.
//
// Reading only the nearest use was the defect: liveness carries a single
// `CausingFact`, so a harmless use sitting between a lifetime-ending event and a
// real dereference answered for both and cancelled the report.
//
// Note on anchors below: the primary warning points at where the borrow was
// created, and the "later used here" note at the NEAREST use after the
// lifetime-ending event -- which in the gap cases is the harmless use, not the
// dereference that makes it an error. The dereference is what is now detected;
// pointing the note at it would mean preferring it as the causing fact, which is a
// separate change.

struct S {
  int id;
  ~S();
};

volatile int sink;
void take(int *);

//===----------------------------------------------------------------------===//
// An OBJECT's lifetime ended while its storage survives (an explicit destructor
// call). The error is specifically FOLLOWING the pointer to the dead object, so a
// use that only reads the pointer value is fine -- but it must not excuse a
// dereference that comes after it.
//===----------------------------------------------------------------------===//

// The controls, which always worked.
void no_gap_read(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();               // expected-note {{invalidated here}}
  sink = p->id;          // expected-note {{later used here}}
}

void no_gap_destroy_twice(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                        // expected-note {{invalidated here}}
  p->~S();                        // expected-note {{later used here}}
}

// A value-only use in between. `p` still designates the destroyed object -- a
// comparison, a discarded read and a null test do not move it -- so the
// dereference that follows is a use of a dead object.
void gap_compare(S *p, S *r) { // expected-warning {{parameter is later invalidated}}
  p->~S();                     // expected-note {{invalidated here}}
  sink = (p == r);             // expected-note {{later used here}}
  sink = p->id;
}

void gap_discarded(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                 // expected-note {{invalidated here}}
  (void)p;                 // expected-note {{later used here}}
  sink = p->id;
}

void gap_null_test(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                 // expected-note {{invalidated here}}
  if (p) {                 // expected-note {{later used here}}
  }
  sink = p->id;
}

void gap_then_destroy_again(S *p, S *r) { // expected-warning {{parameter is later invalidated}}
  p->~S();                                // expected-note {{invalidated here}}
  sink = (p == r);                        // expected-note {{later used here}}
  p->~S();
}

// `e - p` is a distance: it reads both pointers and retargets neither.
void gap_pointer_distance(S *p, S *e) { // expected-warning {{parameter is later invalidated}}
  p->~S();                              // expected-note {{invalidated here}}
  sink = (int)(e - p);                  // expected-note {{later used here}}
  sink = p->id;
}

// `p + 1` computes a new value and leaves `p` designating the dead object; it is
// the RESULT that is retargeted, and the result is a separate origin. Treating the
// read of `p` as a retarget here lost this report.
void gap_non_mutating_arithmetic(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                               // expected-note {{invalidated here}}
  S *q = p + 1;                          // expected-note {{later used here}}
  (void)q;
  sink = p->id;
}

//===----------------------------------------------------------------------===//
// Pointer arithmetic that ASSIGNS to the pointer does excuse what follows: `p` no
// longer designates the object that died, and whether the element it designates
// instead is valid is a bounds question, out of scope for this analysis.
//===----------------------------------------------------------------------===//

void retarget_then_read(S *p) {
  p->~S();
  ++p;
  sink = p->id; // no-warning: a different element
}

void retarget_compound_then_read(S *p) {
  p->~S();
  p += 1;
  sink = p->id; // no-warning
}

// The loop this rule exists for: each iteration destroys a different element.
void destroy_loop(S *begin, S *end) {
  for (S *current = begin; current != end; ++current)
    current->~S();
}

// KNOWN LIMIT: arithmetic whose net displacement is zero is still treated as
// retargeting, so `++p; --p;` and `p += 0;` before a dereference stay silent.
// Recognising them would mean evaluating the displacement; both spellings are
// contrived, and the rule being applied is "the pointer was assigned to, so we no
// longer claim it designates the dead object".

//===----------------------------------------------------------------------===//
// The STORAGE is gone (a scope ended). Holding the borrow at all is the error, so
// every use reports -- a bare read included. The single exception is arithmetic,
// which computes an address and touches nothing.
//===----------------------------------------------------------------------===//

void scope_increment_only() {
  int *p;
  {
    int x = 1;
    p = &x;
  }
  ++p; // no-warning: computes an address, reads no dead memory
}

// Unlike the invalidation case, arithmetic does NOT excuse a later dereference
// here: the storage `++p` lands in is equally gone.
void scope_increment_then_deref() {
  int *p;
  {
    int x = 1;
    p = &x; // expected-warning {{local variable 'x' does not live long enough}}
  }         // expected-note {{destroyed here}}
  ++p;      // expected-note {{later used here}}
  sink = *p;
}

// Liveness flowing to another variable must not lose what the source already
// accumulated. `e`'s only use is arithmetic, and the flow `b -> e` used to
// OVERWRITE `b`'s state with `e`'s, forgetting the dereference of `b` below.
// ASan confirms a stack-use-after-scope here.
void flow_must_not_clobber_source() {
  int *b = nullptr;
  {
    int arr[4]{};
    b = arr;      // expected-warning {{local variable 'arr' does not live long enough}}
  }               // expected-note {{destroyed here}}
  int *e = b + 1; // expected-note {{later used here}}
  ++e;
  sink = *b;
}

// A bare read of a pointer to dead storage still reports: the value is
// indeterminate, and this is also how the rest of the suite spells "used here".
void scope_bare_read() {
  int *p;
  {
    int x = 1;
    p = &x; // expected-warning {{local variable 'x' does not live long enough}}
  }         // expected-note {{destroyed here}}
  (void)p;  // expected-note {{later used here}}
}

// Handing the pointer to a callee, which may follow it.
void scope_pass_to_callee() {
  int *p;
  {
    int x = 1;
    p = &x; // expected-warning {{local variable 'x' does not live long enough}}
  }         // expected-note {{destroyed here}}
  take(p);  // expected-note {{later used here}}
}
