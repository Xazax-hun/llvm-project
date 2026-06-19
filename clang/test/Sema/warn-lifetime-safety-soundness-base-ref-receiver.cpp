// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A non-const method may be called through a base reference/pointer (or a
// ternary, etc.) whose static type lacks the owner field the method reaches by
// downcasting (`static_cast<Derived*>(this)` inside a non-virtual base method).
// The static receiver type at the call site is the owner-less base, and the
// method is not virtual, so neither the static owner gate nor the
// polymorphic-receiver rule fires. The assumed-invalidation is now emitted for
// any record receiver and confirmed in the checker against the loan the receiver
// actually carries: it acts only when the receiver DENOTES a mutable owner (a
// loan it holds points at an object that is-a the receiver's type). This is
// loan-based, so it is robust to references, pointers, and ternaries -- not tied
// to the static receiver type -- while excluding a sub-object receiver that
// merely holds a borrow into a containing owner.

struct [[gsl::Owner(int)]] Buf {
  const int *data() const [[clang::lifetimebound]];
  void grow(); // reallocates
};

struct Base {
  void grow(); // non-virtual; reaches the derived owner by downcast
};
struct Derived : Base {
  Buf buf;
  const int *view() const [[clang::lifetimebound]] { return buf.data(); }
};

int via_reference() {
  Derived d;
  const int *v = d.view(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  Base &b = d;
  b.grow(); // expected-note {{assumed to be invalidated by this operation}}
  return *v;
}

int via_pointer() {
  Derived d;
  const int *v = d.view(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  Base *p = &d;
  p->grow(); // expected-note {{assumed to be invalidated by this operation}}
  return *v;
}

int via_ternary(bool c) {
  Derived d1, d2;
  const int *v = d1.view(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  Base &b = c ? d1 : d2;
  b.grow(); // expected-note {{assumed to be invalidated by this operation}}
  return *v;
}

//===----------------------------------------------------------------------===//
// Negative: a reference to a genuine non-owner object invalidates nothing (the
// receiver does not denote an owner).
//===----------------------------------------------------------------------===//
struct Plain {
  int n;
  const int *get() const [[clang::lifetimebound]] { return &n; }
  void bump() { n++; }
};
int ok_plain() {
  Plain p;
  const int *v = p.get();
  Plain &r = p;
  r.bump(); // no-warning: receiver denotes no owner
  return *v;
}
