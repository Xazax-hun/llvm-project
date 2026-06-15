// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// The non-invalidating read-accessor allow-list (operator[], at, data, find,
// ...) must apply only to genuine std-library owner types. A custom
// [[gsl::Owner]] placed in a reserved-style namespace (`__detail`, which the STL
// namespace heuristic treated as std) with a non-const, reallocating method
// merely NAMED like a read accessor was wrongly exempted from assumed
// invalidation, so a borrow into it dangled silently after the "accessor" call.
// The exemption now requires a genuine `std` ancestor.

namespace __detail {
struct [[gsl::Owner(int)]] Buf {
  const int *begin() const [[clang::lifetimebound]];
  int data(); // non-const, allow-listed NAME, but a user type -> may invalidate
  int grow(); // non-allow-listed name (control)
};
} // namespace __detail

// A method named like an accessor (`data`) on a custom owner is assumed to
// invalidate -- the borrow taken before it dangles.
void accessor_named_method() {
  __detail::Buf b;
  const int *p = b.begin(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  b.data();                 // expected-note {{assumed to be invalidated by this operation}}
  (void)*p;
}

// Control: a non-allow-listed name was already assumed-invalidating; still is.
void non_accessor_named_method() {
  __detail::Buf b;
  const int *p = b.begin(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  b.grow();                 // expected-note {{assumed to be invalidated by this operation}}
  (void)*p;
}

// Negative: a const method genuinely cannot mutate -- no invalidation.
namespace __detail {
struct [[gsl::Owner(int)]] Buf2 {
  const int *begin() const [[clang::lifetimebound]];
  int peek() const; // const -> non-invalidating
};
} // namespace __detail
void const_method_ok() {
  __detail::Buf2 b;
  const int *p = b.begin();
  b.peek();
  (void)*p; // no-warning
}
