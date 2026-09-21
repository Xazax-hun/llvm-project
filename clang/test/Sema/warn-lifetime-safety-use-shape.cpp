// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety -verify %s

// WHAT COUNTS AS USING A POINTEE.
//
// `p->m`, `*p` and `p[i]` do not touch anything -- they compute a location.
// `&p->m` proves it, and the generator emits IDENTICAL facts for `sink = p->id`
// and `int *q = &p->id`. The object is touched by the LOAD or the STORE applied
// to the resulting lvalue, so that is where the access is recorded: at the
// lvalue-to-rvalue conversion for a read, at the assignment destination for a
// write. Everything else is assumed to touch the pointee -- any call, including
// a member call, whose body is presumed to load it.
//
// A read of the pointer VALUE alone -- `++p`, `p += n`, `p == q`, `if (p)` -- is
// not a use at all. It neither touches the pointee nor needs the borrow kept
// alive, because whatever accesses the pointee later records its own access. It
// asserts nothing about what the pointer designates AFTERWARDS either, so it
// never cancels a later access: believing `++p` lands on a valid element
// produced a hole every time it was tried.
//
// The question is only ASKED where an object's lifetime ended while its storage
// survives -- an explicit destructor call -- since that is the one case where
// merely holding the pointer is still fine. Where the storage itself is gone,
// holding the borrow at all is the error and every use reports.

struct S {
  int id;
  ~S();
};

volatile int sink;
void take(int *);

//===----------------------------------------------------------------------===//
// Storage survives: an explicit destructor call.
//===----------------------------------------------------------------------===//

void read_after_destroy(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                      // expected-note {{invalidated here}}
  sink = p->id;                 // expected-note {{later used here}}
}

void destroy_twice(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                 // expected-note {{invalidated here}}
  p->~S();                 // expected-note {{later used here}}
}

// A use that only reads the pointer value in between must not answer for the
// dereference that follows -- reading just the nearest use let it cancel the
// report. The note points at the access, not at the nearer harmless read.
void gap_compare(S *p, S *r) { // expected-warning {{parameter is later invalidated}}
  p->~S();                     // expected-note {{invalidated here}}
  sink = (p == r);
  sink = p->id; // expected-note {{later used here}}
}

void gap_discarded(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                 // expected-note {{invalidated here}}
  (void)p;
  sink = p->id; // expected-note {{later used here}}
}

void gap_null_test(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                 // expected-note {{invalidated here}}
  if (p) {
  }
  sink = p->id; // expected-note {{later used here}}
}

void gap_then_destroy_again(S *p, S *r) { // expected-warning {{parameter is later invalidated}}
  p->~S();                                // expected-note {{invalidated here}}
  sink = (p == r);
  p->~S(); // expected-note {{later used here}}
}

// POSTFIX `p++` evaluates to the pointer from BEFORE the increment -- the object
// that just died -- so the load through it reaches the dead object. This is the
// cursor idiom, silent while arithmetic could cancel a dereference.
void postfix_yields_old_value(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                            // expected-note {{invalidated here}}
  sink = (p++)->id;                   // expected-note {{later used here}}
}

// A STORE touches the pointee exactly as a load does, and is spelled with no
// conversion at all.
void store_after_destroy(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                       // expected-note {{invalidated here}}
  p->id = 1;                     // expected-note {{later used here}}
}

// Reading the pointer value is not an error here: the storage is still there, so
// the pointer is still a valid pointer.
void bare_compare(S *p, S *r) {
  p->~S();
  sink = (p == r); // no-warning
}

void bare_increment(S *p) {
  p->~S();
  ++p; // no-warning
}

// DELIBERATE FALSE POSITIVE: `++p` moves to a different, live element. Silence
// would require believing the new element is valid.
void prefix_then_read(S *p) { // expected-warning {{parameter is later invalidated}}
  p->~S();                    // expected-note {{invalidated here}}
  ++p;
  sink = p->id; // expected-note {{later used here}}
}

// DELIBERATE FALSE POSITIVE, and really the known element-identity gap: every
// element of `c` shares one loan, so the model cannot see that each iteration
// destroys a different object. The note points at the destructor call in the
// BODY -- the use that reaches the object -- not at the loop header.
void destroy_loop(S *begin, S *end) { // expected-warning {{parameter is later invalidated}}
  for (S *c = begin; c != end; ++c)
    c->~S(); // expected-note {{invalidated here}} \
             // expected-note {{later used here}}
}

//===----------------------------------------------------------------------===//
// Storage is gone: a scope ended. An address computed from dead storage is still
// dead storage, so nothing here is exempt except reads of the pointer value.
//===----------------------------------------------------------------------===//

void scope_deref() {
  int *p;
  {
    int x = 1;
    p = &x;  // expected-warning {{local variable 'x' does not live long enough}}
  }          // expected-note {{destroyed here}}
  sink = *p; // expected-note {{later used here}}
}

// Reading only the pointer value touches nothing -- the same rule as above, not
// a special case for expiry.
void scope_increment_only() {
  int *p;
  {
    int x = 1;
    p = &x;
  }
  ++p; // no-warning
}

void scope_compare_only(int *q) {
  int *p;
  {
    int x = 1;
    p = &x;
  }
  sink = (p == q); // no-warning
}

// The shapes an arithmetic exemption used to silence, each an ASan-confirmed
// stack-use-after-scope.
void scope_postfix_arrow() {
  S *p;
  {
    S a[4]{};
    p = a; // expected-warning {{local variable 'a' does not live long enough}}
  }        // expected-note {{destroyed here}}
  sink = (p++)->id; // expected-note {{later used here}}
}

void scope_prefix_arrow() {
  S *p;
  {
    S a[4]{};
    p = a; // expected-warning {{local variable 'a' does not live long enough}}
  }        // expected-note {{destroyed here}}
  sink = (++p)->id; // expected-note {{later used here}}
}

void scope_cursor_store() {
  S *w;
  {
    S b[8]{};
    w = b; // expected-warning {{local variable 'b' does not live long enough}}
  }        // expected-note {{destroyed here}}
  w++->id = 1; // expected-note {{later used here}}
}

void scope_bare_read() {
  int *p;
  {
    int x = 1;
    p = &x; // expected-warning {{local variable 'x' does not live long enough}}
  }         // expected-note {{destroyed here}}
  (void)p;  // expected-note {{later used here}}
}

void scope_pass_to_callee() {
  int *p;
  {
    int x = 1;
    p = &x; // expected-warning {{local variable 'x' does not live long enough}}
  }         // expected-note {{destroyed here}}
  take(p);  // expected-note {{later used here}}
}

// Liveness flowing to another variable must not lose what the source
// accumulated: the flow `b -> e` used to OVERWRITE `b`'s state with `e`'s,
// forgetting the dereference of `b` below. ASan confirms this one.
void flow_must_not_clobber_source() {
  int *b = nullptr;
  {
    int arr[4]{};
    b = arr; // expected-warning {{local variable 'arr' does not live long enough}}
  }               // expected-note {{destroyed here}}
  int *e = b + 1;
  ++e;
  sink = *b; // expected-note {{later used here}}
}
