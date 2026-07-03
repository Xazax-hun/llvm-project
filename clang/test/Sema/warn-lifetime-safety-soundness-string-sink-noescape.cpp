// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

// std::basic_string owns its buffer and never retains a borrow from an argument:
// it copies (ctor/assign/append/operator=/...) or only reads the characters of a
// string-source argument (std::string_view / const char* / std::string). So such
// an argument does not escape and must NOT be flagged as an unannotated
// indirection -- even though a std::string_view itself can hold a borrow. A
// genuinely unannotated user sink is still flagged.

namespace std {
template <class T> struct basic_string_view {
  basic_string_view(const T *);
};
using string_view = basic_string_view<char>;
template <class T> struct basic_string {
  basic_string(const char *);
  basic_string(basic_string_view<T>);
  basic_string<T> &operator=(const char *);
  basic_string<T> &append(basic_string_view<T>);
  basic_string<T> &assign(basic_string_view<T>);
  operator basic_string_view<T>() const;
};
using string = basic_string<char>;
} // namespace std

void user_sink(std::string_view v); // unannotated: NOT exempt

void demo() {
  std::string src("hello");
  std::string_view sv = src; // borrows src (alive)

  // All of these copy/read the characters; sv does not escape -> no diagnostic.
  std::string owned(sv);
  owned.append(sv);
  owned.assign(sv);
  owned = "literal";

  user_sink(sv); // expected-warning {{not annotated for lifetime safety}}
}
