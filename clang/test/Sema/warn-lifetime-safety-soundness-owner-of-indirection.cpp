// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-owner-of-indirection -verify %s

// Under the safe programming model a [[gsl::Owner]] container whose element
// type is an indirection (a pointer, reference, or [[gsl::Pointer]]) is
// rejected: the analysis does not track borrows held by individual elements,
// so a borrow stored into an element could otherwise dangle unnoticed.

namespace std {
template <class T> struct [[gsl::Owner]] vector {
  vector();
};
template <class T, unsigned N> struct [[gsl::Owner]] array {};
template <class K, class V> struct [[gsl::Owner]] map {};
struct [[gsl::Owner]] string {};
struct [[gsl::Pointer]] string_view {};
} // namespace std

// Reject: element type is an indirection.
void local_vector_of_ptr() {
  std::vector<int *> v; // expected-warning {{type 'std::vector<int *>' is a container whose element type is a pointer or reference; lifetime safety cannot track borrows held by its elements}}
  (void)v;
}

void local_array_of_ptr() {
  std::array<int *, 4> a; // expected-warning {{is a container whose element type is a pointer or reference}}
  (void)a;
}

void local_vector_of_view() {
  std::vector<std::string_view> v; // expected-warning {{is a container whose element type is a pointer or reference}}
  (void)v;
}

void local_map_value_ptr() {
  std::map<int, int *> m; // expected-warning {{is a container whose element type is a pointer or reference}}
  (void)m;
}

// Recurses into owner template arguments.
void nested() {
  std::vector<std::vector<int *>> v; // expected-warning {{is a container whose element type is a pointer or reference}}
  (void)v;
}

// Parameters, by value and by reference (the latter seen through the reference;
// holds regardless of any annotation since the elements are untracked).
void param_byval(std::vector<int *> v) { (void)v; } // expected-warning {{is a container whose element type is a pointer or reference}}
void param_byref(std::vector<int *> &v) { (void)v; } // expected-warning {{is a container whose element type is a pointer or reference}}

// Call returning such a container.
std::vector<int *> make();
void call_return() {
  make(); // expected-warning {{is a container whose element type is a pointer or reference}}
}

// Accept: element type is not an indirection.
void ok() {
  std::vector<int> a;
  std::vector<std::string> b;
  std::vector<std::vector<int>> c;
  (void)a;
  (void)b;
  (void)c;
}

// Per-construct opt-out.
void opt_out() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-owner-of-indirection"
  std::vector<int *> v; // no-warning
  (void)v;
#pragma clang diagnostic pop
}
