// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-const-subversion -verify %s

// The analysis assumes a const member function does not invalidate borrows into
// the object. A 'mutable' field or a 'const_cast' can subvert that assumption
// (a const method could reallocate/free state through them), so the safe
// programming model rejects both. Without these checks, a const method that
// invalidates via 'mutable'/'const_cast' is a silent false negative.

struct WithMutable {
  mutable int *data; // expected-warning {{'mutable' field 'data' can be modified by a const member function; lifetime safety assumes const member functions do not invalidate borrows into the object}}
  int normal;
};

struct WithConstCast {
  int *data;
  void reset() const {
    const_cast<WithConstCast *>(this)->data = nullptr; // expected-warning {{'const_cast' can subvert the const-correctness that lifetime safety relies on to assume const member functions do not invalidate borrows into the object}}
  }
};

const int global = 0;
int *strip_global() {
  return const_cast<int *>(&global); // expected-warning {{'const_cast' can subvert the const-correctness}}
}

// No 'mutable' / no 'const_cast' -> clean.
struct Clean {
  int *data;
  int *get() const { return data; }
};

// Per-construct opt-out.
struct OptOut {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-const-subversion"
  mutable int *cache; // no-warning
#pragma clang diagnostic pop
};
