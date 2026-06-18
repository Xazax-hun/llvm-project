// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A range-based for loop over a standard container -- the most common loop --
// must be clean under the safe programming model. Two things make it so:
//   * comparison/relational operators (the iterator '__begin != __end' test)
//     do not capture their operands and are exempt from the unannotated
//     indirection check, and
//   * begin()/end() are lifetimebound (or GSL-tracked), so the iterator keeps
//     the container's borrows.
// Separately, STL container constructors copy the values they are given in, so
// passing a value (or a pointer/reference to a non-borrow-holding type, e.g.
// 'const char *' to std::string) does not escape.

namespace std {
template <class T> struct [[gsl::Pointer]] iter {
  T *p;
  T &operator*() const;
  iter &operator++();
  // Comparison operators: passing another iterator (a gsl::Pointer) here must
  // not be flagged as an unannotated indirection.
  bool operator==(const iter &) const;
  bool operator!=(const iter &) const;
};

template <class T> struct [[gsl::Owner]] vector {
  vector();
  explicit vector(const T &fill); // value constructor: copies 'fill' in
  iter<T> begin() const [[clang::lifetimebound]];
  iter<T> end() const [[clang::lifetimebound]];
};

template <class C> struct [[gsl::Owner]] basic_string {
  basic_string(const C *); // copies the characters in; pointer does not escape
};
using string = basic_string<char>;

// A view: a gsl::Pointer that captures what it is built from.
struct [[gsl::Pointer]] string_view {
  string_view(const char *); // captures the pointer
};
} // namespace std

//===----------------------------------------------------------------------===//
// Range-based for loops are clean.
//===----------------------------------------------------------------------===//

int loop_read(const std::vector<int> &v [[clang::noescape]]) {
  int s = 0;
  for (int x : v) // no-warning
    s += x;
  return s;
}

void loop_local() {
  std::vector<int> v;
  for (int x : v) // no-warning
    (void)x;
}

//===----------------------------------------------------------------------===//
// STL container constructors copy their input in.
//===----------------------------------------------------------------------===//

void string_from_literal() {
  std::string a = "abc"; // no-warning
  std::string b("xyz");  // no-warning
  std::string c{"q"};    // no-warning
  (void)a;
  (void)b;
  (void)c;
}

void vector_value_ctor() {
  int x = 0;
  std::vector<int> v(x); // no-warning: 'x' is copied into the container
  (void)v;
}

//===----------------------------------------------------------------------===//
// The exemption is narrow: it does not hide real captures.
//===----------------------------------------------------------------------===//

// A gsl::Pointer (view) is not an STL container; building one from a pointer
// captures it. The capture is modeled by GSL construction tracking, and the
// unannotated parameter it flows from is still surfaced at the definition.
std::string_view view_from_ptr(const char *p) { // expected-warning {{parameter that can hold a borrow is not annotated for lifetime safety}} \
                                                 // expected-warning {{parameter in intra-TU function should be marked [[clang::lifetimebound]]}}
  return p; // expected-note {{param returned here}}
}

// A non-STL user type constructor is not exempt either.
struct Box {
  Box(const int &r); // captures &r in general
};
void user_ctor() {
  int x = 0;
  Box b(x); // expected-warning {{argument is bound to a parameter that can hold a borrow but is not annotated for lifetime safety}}
  (void)b;
}
