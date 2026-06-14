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
  int *data() [[clang::lifetimebound]]; // User accessor: NOT recognized as
                                        // non-invalidating (only std accessors
                                        // are), so assumed to invalidate.
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

// A non-const method on a user owner is not on the std non-invalidating
// allow-list, so it is conservatively assumed to invalidate -- even a
// lifetimebound accessor (we cannot tell a user mutator from a user accessor).
void user_accessor_warns() {
  MyBuf b;
  int *p = b.data(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  int *q = b.data(); // expected-note {{assumed to be invalidated by this operation}}
  (void)p;
  (void)q;
}

// A recognized std accessor (operator[]) does NOT invalidate: a borrow held
// across another such access stays valid.
void std_accessor_silent() {
  vector<int> v;
  int &a = v[0];
  int &b = v[0]; // no-warning (std operator[] is on the allow-list)
  (void)a;
  (void)b;
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
  Str *p = &base[0];
  MutSpan<Str> s(base);
  mutate_span(s); // expected-note {{assumed to be invalidated by this operation}}
  (void)p;
}

void const_owner_span_silent(Str *base) {
  Str *p = &base[0];
  ConstSpan<Str> s(base);
  read_span(s); // no-warning
  (void)p;
}


//===----------------------------------------------------------------------===//
// Case 3: calling a lambda that captures an owner by reference. A by-reference
// capture gives the closure non-const access to the owner, so calling it is
// assumed to mutate the owner (like passing it to a non-const ref parameter).
//===----------------------------------------------------------------------===//

void lambda_ref_capture_warns() {
  vector<int> v;
  int &r = v[0]; // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  auto grow = [&v]() { v.push_back(1); };
  grow(); // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

void lambda_by_value_silent() {
  vector<int> v;
  int &r = v[0];
  auto grow = [v]() mutable { v.push_back(1); }; // by value: a copy
  grow();                                        // no-warning
  (void)r;
}

void lambda_no_capture_silent() {
  vector<int> v;
  int &r = v[0];
  auto noop = []() {};
  noop(); // no-warning
  (void)r;
}

//===----------------------------------------------------------------------===//
// A record that reaches a mutable owner through a non-const pointer/reference
// member (not only a by-value owner field) is treated as containing a mutable
// owner: a non-const method that mutates the owner through the indirection is
// assumed to invalidate borrows into that owner. Because the wrapper is a
// gsl::Pointer, the borrows it aliases live on its pointee origin, so the
// invalidation also reaches a borrow taken *directly* from the underlying owner
// (which carries the owner's loan, not the wrapper object's).
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] PtrWrap {
  vector<int> *v;
  PtrWrap(vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
  void grow() {
    for (int i = 0; i < 1000; ++i)
      v->push_back(i);
  }
};

void ptr_member_owner_invalidated() {
  vector<int> d;
  d.push_back(42);
  PtrWrap w(&d);
  int &r = d[0]; // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  w.grow();      // expected-note {{assumed to be invalidated by this operation}}
  (void)r;
}

// A const pointee cannot be reallocated through the member, so no invalidation.
struct ConstPtrWrap {
  const vector<int> *v;
  ConstPtrWrap(const vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
  const int *read() const { return v->data(); }
  void touch() {} // non-const, but cannot mutate *v
};

void const_ptr_member_silent() {
  vector<int> d;
  d.push_back(42);
  ConstPtrWrap w(&d);
  const int *x = w.read();
  w.touch(); // no-warning
  (void)x;
}
