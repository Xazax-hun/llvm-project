// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-assumed-invalidation -verify %s

#include "Inputs/lifetime-analysis.h"

using std::vector;

// The analysis conservatively assumes that operations it cannot prove leave an
// owner unchanged invalidate borrows into that owner: non-const member calls,
// and passing an owner to a non-const pointer/reference parameter. The warning
// only fires when a borrow into the owner is actually live across the operation.

//===----------------------------------------------------------------------===//
// Case 1: non-const member call on an owner.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(int)]] MyBuf {
  int *data() [[clang::lifetimebound]]; // Accessor (annotated): not invalidating.
  void touch();                         // Non-const, non-accessor.
  void look() const;
};

void member_call_warns() {
  MyBuf b;
  int *p = b.data(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  b.touch();         // expected-note {{assumed to be invalidated by this operation}}
  (void)p;
}

void const_member_silent() {
  MyBuf b;
  int *p = b.data();
  b.look(); // no-warning
  (void)p;
}

void accessor_silent() {
  MyBuf b;
  int *p = b.data();
  int *q = b.data(); // no-warning (lifetimebound accessor does not invalidate)
  (void)p;
  (void)q;
}

//===----------------------------------------------------------------------===//
// Case 2: passing an owner to a non-const pointer/reference parameter.
//===----------------------------------------------------------------------===//

void mutate(vector<int> &v);
void inspect(const vector<int> &v);

void nonconst_owner_ref_warns() {
  vector<int> v;
  int &r = v[0]; // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  mutate(v);     // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

void const_owner_ref_silent() {
  vector<int> v;
  int &r = v[0];
  inspect(v); // no-warning
  (void)r;
}

void no_outstanding_borrow_silent() {
  vector<int> v;
  mutate(v); // no-warning
}

// A borrow invalidated by a known mutator is reported precisely (under
// -Wlifetime-safety-invalidation); the lower-confidence assumed-invalidation
// warning is suppressed for it even if a later non-const operation occurs.
void known_invalidation_suppresses_assumed() {
  vector<int> v;
  int &r = v[0];
  v.push_back(1);
  mutate(v); // no-warning
  (void)r;
}

//===----------------------------------------------------------------------===//
// Case 3: passing a gsl::Pointer (e.g. a span) that exposes mutable access to a
// non-const owner pointee can invalidate borrows into that owner.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(int)]] Str {
  int *data() [[clang::lifetimebound]];
};
template <class T> struct [[gsl::Pointer]] MutSpan {
  MutSpan(T *);
  T &operator[](int) const; // mutable owner element access
};
template <class T> struct [[gsl::Pointer]] ConstSpan {
  ConstSpan(const T *);
  const T &operator[](int) const; // const element access
};

void mutate_span(MutSpan<Str> s);
void read_span(ConstSpan<Str> s);

void mutable_owner_span_warns(Str *base) { // expected-warning {{parameter may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  MutSpan<Str> s(base);
  int *p = base[0].data();
  mutate_span(s); // expected-note {{assumed to be invalidated by this operation}}
  (void)p;
}

void const_owner_span_silent(Str *base) {
  ConstSpan<Str> s(base);
  int *p = base[0].data();
  read_span(s); // no-warning
  (void)p;
}

