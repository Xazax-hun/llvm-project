// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-const-subversion -verify %s

// A const method that mutates its owner by invoking a callable data member.
// Assumed-invalidation only fires for non-const calls, so invoking a callable
// member -- whose stored closure may have captured the object in a *different*
// function (here the constructor's [this]-capturing lambda) -- from a const
// method used to subvert const undetected: a view returned by a lifetimebound
// accessor dangled after the closure mutated/reallocated the buffer. Invoking a
// callable data member is now treated as a possible mutation of the enclosing
// object regardless of const, so the const method is flagged as const-subverting.

namespace std {
template <class T> struct basic_string {
  basic_string(const char *);
  void push_back(char);
  const T *data() const;
  ~basic_string();
};
using string = basic_string<char>;
template <class T> struct basic_string_view {
  basic_string_view(const basic_string<T> &);
  const T *data() const;
};
using string_view = basic_string_view<char>;
template <class> class function;
template <class R, class... Args> class function<R(Args...)> {
public:
  template <class F> function(F) {}
  R operator()(Args...) const;
  ~function();
};
} // namespace std

struct [[gsl::Owner(std::string)]] MyOwner {
private:
  std::string buf = "initial contents";
  std::function<void()> grower;

public:
  MyOwner() : grower([this] { buf.push_back('b'); }) {} // captures [this]
  std::string_view view() const [[clang::lifetimebound]] { return buf; }
  // const, yet mutates buf by invoking the [this]-capturing closure.
  void poke() const {
    grower(); // expected-warning {{const}}
  }
};
