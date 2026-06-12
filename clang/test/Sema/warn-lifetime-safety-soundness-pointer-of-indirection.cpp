// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-pointer-of-indirection -verify %s
//
// (Membership in -Wlifetime-safety-soundness is covered by the soundness corpus;
// here we isolate the flag so an uninitialized view's expected -lost-loan does
// not add noise.)

// Under the safe programming model a [[gsl::Pointer]] view whose pointee/element
// type is itself an indirection (a pointer, reference, or [[gsl::Pointer]]) is
// rejected: the view tracks only its outer level, so a borrow held one level
// deeper -- the pointee of `*view` -- could dangle unnoticed. This is the
// gsl::Pointer analogue of -Wlifetime-safety-owner-of-indirection.

namespace std {
template <class T> struct [[gsl::Owner]] vector {
  vector();
};
struct [[gsl::Owner]] string {};
struct [[gsl::Pointer]] string_view {
  using value_type = char;
};
// A view with a value_type / element_type typedef (std::span-like).
template <class T> struct [[gsl::Pointer]] span {
  using element_type = T;
  T &operator[](int) const;
};
// An iterator-like view: a [[gsl::Pointer]] whose sole template argument is a
// pointer (`int*`), but whose value_type is the *element* (not a pointer). Must
// NOT be flagged -- this is exactly std::vector<int>::iterator's shape.
template <class Ptr> struct [[gsl::Pointer]] wrap_iter {
  using value_type = int;
  Ptr ptr;
};
} // namespace std

// Reject: pointee/element type is an indirection.
void local_span_of_ptr() {
  std::span<int *> s; // expected-warning {{type 'std::span<int *>' is a view whose pointee type holds a borrow (a pointer, reference, or unannotated borrow-holding type); lifetime safety cannot track borrows held one level below the view}}
  (void)s;
}

void local_span_of_view() {
  std::span<std::string_view> s; // expected-warning {{is a view whose pointee type holds a borrow}}
  (void)s;
}

void param_span_of_ptr(std::span<int *> s [[clang::noescape]]) { // expected-warning {{is a view whose pointee type holds a borrow}}
  (void)s;
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A view over a non-indirection element is fine.
void local_span_of_int() {
  std::span<int> s; // no-warning
  (void)s;
}

void local_span_of_owner() {
  std::span<std::string> s; // no-warning (element is an owner, not an indirection)
  (void)s;
}

// A plain view.
void local_string_view() {
  std::string_view sv; // no-warning
  (void)sv;
}

// An iterator whose value_type is the element (not a pointer) must not be
// flagged even though its template argument is a pointer.
void local_iterator() {
  std::wrap_iter<int *> it; // no-warning
  (void)it;
}

//===----------------------------------------------------------------------===//
// Fallback: a custom view without value_type/element_type uses operator*.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner]] Res {};
template <class T> struct [[gsl::Pointer]] View {
  T *p;
  T &operator*() const;
};

void custom_view_of_ptr() {
  View<int *> v; // expected-warning {{is a view whose pointee type holds a borrow}}
  (void)v;
}

void custom_view_of_owner() {
  View<Res> v; // no-warning (pointee is an owner)
  (void)v;
}

//===----------------------------------------------------------------------===//
// The pointee can also be declared via the [[gsl::Pointer(T)]] attribute's
// optional type argument (rather than a typedef / template argument).
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer(int *)]] AttrViewOfPtr {
  int **pp;
};
void attr_view_of_ptr() {
  AttrViewOfPtr v; // expected-warning {{is a view whose pointee type holds a borrow}}
  (void)v;
}

struct [[gsl::Pointer(char)]] AttrViewOfChar {
  const char *p;
};
void attr_view_of_value() {
  AttrViewOfChar v; // no-warning (pointee is not an indirection)
  (void)v;
}

// A pointer-of-indirection used as a DATA MEMBER is rejected at the class
// definition, mirroring the local/parameter checks.
struct HasViewOfIndirectionMember {
  View<int *> v; // expected-warning {{is a view whose pointee type holds a borrow}}
};
