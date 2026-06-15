// Minimal std-aggregate stand-ins for the lifetime-safety aggregate-of-
// indirection test. Included via -isystem so these declarations are treated as
// living in a system header (their own field-declaration diagnostics are
// suppressed), mirroring how std::pair / std::tuple actually reach user code.
#pragma once

namespace std {
template <class T> struct [[gsl::Owner]] vector {
  vector();
};
struct [[gsl::Owner]] string {};
struct [[gsl::Pointer]] string_view {};
template <class T> struct [[gsl::Pointer]] span {};
template <class T> struct default_delete {};
template <class T, class D = default_delete<T>> struct [[gsl::Owner]] unique_ptr {
  unique_ptr();
};

// Non-owner aggregates (neither [[gsl::Owner]] nor [[gsl::Pointer]]) with public
// data members -- the std::pair / std::tuple shape that wraps an element of
// owner-/pointer-of-indirection type.
template <class A, class B> struct pair {
  A first;
  B second;
};
template <class... Ts> struct tuple {};
} // namespace std
