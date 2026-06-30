// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-const-subversion -verify %s

// The const-subversion check trusts not only `this` in a const member function,
// but any const-reference/pointer parameter: an indirection to a const value the
// caller assumes will not be mutated behind its back. Mutating a mutable owner
// reached through a pointer member of such a parameter (shallow const -- `const`
// does not propagate through the smart pointer) is flagged, just like the
// const-member-function case.

struct [[gsl::Owner(char)]] Buf {
  void mutate();    // non-const mutator
  int read() const; // const accessor
};
// An owning smart pointer with const operator*/operator-> handing out a
// non-const pointee, exactly like std::unique_ptr.
template <class T> struct [[gsl::Owner]] Ptr {
  T &operator*() const;
  T *operator->() const;
};
struct Holder { Ptr<Buf> buf; };

// const& parameter: mutate *buf through the smart-pointer member.
void via_const_ref(const Holder &x) {
  x.buf->mutate(); // expected-warning {{mutating an owner through a pointer member of a const-qualified object or parameter}}
}

// const* parameter.
void via_const_ptr(const Holder *x) {
  x->buf->mutate(); // expected-warning {{mutating an owner through a pointer member of a const-qualified object or parameter}}
}

// Nested fields: the parameter's loan propagates through each member access.
struct Outer { Holder h; };
void via_nested(const Outer &x) {
  x.h.buf->mutate(); // expected-warning {{mutating an owner through a pointer member of a const-qualified object or parameter}}
}

// Control: a non-const reference parameter may legitimately be mutated.
void via_mutable_ref(Holder &x) {
  x.buf->mutate(); // no-warning
}

// Control: a by-value const parameter is a copy; the analysis treats it as an
// independent object, so mutating its pointee is not a const subversion here.
void via_value(const Holder x) {
  x.buf->mutate(); // no-warning
}

// Control: mutating a local (not reached through the const parameter) is fine,
// even with a same-typed const parameter in scope.
void via_local(const Holder &x) {
  Holder local;
  local.buf->mutate(); // no-warning
}
