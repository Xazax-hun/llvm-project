// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-return-stack-address -verify %s

// A gsl::Pointer view constructed from a (pointer, size) pair -- e.g.
// 'std::span<T>(ptr, count)' -- borrows from the pointer argument. When that
// constructor parameter is [[clang::lifetimebound]] (as it is, inferred, for the
// standard views), the borrow must flow into the constructed view so that
// returning the view bound to 'this'/a parameter is verified and a dangling
// view is caught. (Previously the multi-argument constructor form was not
// modeled, so 'lifetimebound' on such a method could not be verified.)

struct [[gsl::Pointer]] IntSpan {
  const int *p_;
  IntSpan(const int *first [[clang::lifetimebound]], unsigned n);
  const int *begin() const;
  const int *end() const;
};

struct [[gsl::Owner]] Container {
  int buf_[8];
  // The returned span borrows 'this'->buf_; lifetimebound is now verifiable.
  IntSpan cell(unsigned n) const [[clang::lifetimebound]] {
    return IntSpan(buf_, n); // no-warning
  }
};

// Soundness preserved: a span built over a local buffer and returned dangles.
IntSpan dangling_return() {
  int a[4] = {};
  return IntSpan(a, 4); // expected-warning {{stack memory associated with local variable 'a' is returned}}
                        // expected-note@-1 {{returned here}}
}

// Soundness preserved: a span over a buffer that has gone out of scope.
int dangling_use() {
  IntSpan s(nullptr, 0);
  {
    int a[4] = {1, 2, 3, 4};
    s = IntSpan(a, 4); // expected-warning {{local variable 'a' does not live long enough}}
  } // expected-note {{destroyed here}}
  return *s.begin(); // expected-note {{later used here}}
}
