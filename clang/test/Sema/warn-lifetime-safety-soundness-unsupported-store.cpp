// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-gnu-conditional-omitted-operand -verify %s

// An assignment whose destination lvalue selects/forwards among several objects
// -- a conditional `(c ? p : q) = ...`, a comma `(f(), p) = ...`, the GNU
// `(p ?: q) = ...`, or those wrapped in `*&(...)`/casts -- has no single
// statically-known destination origin, and used to be rejected wholesale.
//
// The destinations are now taken from the loans that lvalue itself holds, which
// name each candidate object, so these stores are TRACKED (merging into every
// candidate, since which one was written is unknown). The rejection remains for
// a destination whose loans do not resolve to storage -- see
// `unresolved_subobject` at the end.
//
// Storing a non-dangling value is therefore silent now; the companion soundness
// checks live in warn-lifetime-safety-soundness-selecting-lvalue-store.cpp.

int g;
int side();

// Conditional pointer lvalue store -- rejected (robust to the spelling).
void cond(bool c) {
  int *p = &g, *q = &g;
  (c ? p : q) = &g; // no-warning: routed to p and q
}

// `*&(...)` wrapper around the conditional -- also rejected (no enumeration).
void deref_addrof(bool c) {
  int *p = &g, *q = &g;
  (*&(c ? p : q)) = &g; // expected-warning {{uses more than one level of indirection}}
}

// Comma lvalue store -- rejected.
void comma() {
  int *p = &g;
  (side(), p) = &g; // no-warning: routed
}

// GNU binary conditional lvalue store -- rejected.
void gnu_cond() {
  int *p = &g, *q = &g;
  (p ?: q) = &g; // no-warning: routed
}

// Negative: the same conditional with a NON-borrow (int) destination is fine.
void cond_int(bool c) {
  int x, y;
  (c ? x : y) = 5; // no-warning
}

// Negative: a user operator[] storing a non-borrow (char) is fine -- the
// destination type (char) holds no borrow, so the unroutable operator[] LHS is
// not flagged.
struct CharVec {
  char &operator[](unsigned) [[clang::lifetimebound]];
};
void op_char(CharVec &v [[clang::noescape]], unsigned i) {
  v[i] = 'a'; // no-warning
}

// Negative: a plain routable store stays silent.
void plain() {
  int *p = &g;
  p = &g; // no-warning
}

// A view-member store whose base SELECTS among objects -- `(c ? a : b).p = ...`
// -- is also rejected: the base is a transient origin that does not root in a
// single stable object, so the merge into the view's own origin would be
// dropped. (A non-selecting nested base like `v.inner.p` is tracked instead;
// see warn-lifetime-safety-soundness-view-member-store.cpp.)
struct [[gsl::Pointer]] View {
  const int *p;
};
void cond_view_base(View a [[clang::noescape]], View b [[clang::noescape]],
                    bool c) {
  int local = 0;
  // Still refused, and not by the routing: a store into a member whose BASE is a
  // transient selecting origin is refused in the fact generator, before any
  // dynamic store is emitted, so no routing happens for it at all.
  (c ? a : b).p = &local; // expected-warning {{assignment through this expression is not modeled}}
}
