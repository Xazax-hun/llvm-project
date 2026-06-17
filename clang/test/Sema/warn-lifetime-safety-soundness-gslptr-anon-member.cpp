// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A [[gsl::Pointer]] is a leaf in the origin tree (its members are not tracked
// per field), so a store into a borrow-holding member is merged into the view's
// own origin (rounds 26/28) -- but that merge was gated on the assigned member's
// immediate base being a gsl::Pointer. When the member lives in an ANONYMOUS
// struct, the base is the unnamed anonymous-record subobject (whose type is not
// a gsl::Pointer), so the merge was skipped, the stored borrow dropped, and a
// later dangling read silently missed. The gate now peels anonymous-record bases
// to reach the real enclosing gsl::Pointer object.

struct [[gsl::Pointer]] V {
  struct {
    int *p;
  }; // anonymous struct holds the borrow
  V() { p = nullptr; }
};

int anon_struct_member(V v [[clang::noescape]]) {
  {
    int local = 0;
    v.p = &local; // expected-warning {{local variable 'local' does not live long enough}}
  } // expected-note {{destroyed here}}
  return *v.p; // expected-note {{later used here}}
}

// Doubly-nested anonymous struct: the gate sees through it, but the deeper
// member-access origin isn't a stable mergeable target, so it falls back to the
// unsupported-store catch-all -- still caught (non-silent), just less precise.
struct [[gsl::Pointer]] W {
  struct {
    struct {
      int *q;
    };
  };
  W() { q = nullptr; }
};
int nested_anon_member(W w [[clang::noescape]]) {
  {
    int local = 0;
    w.q = &local; // expected-warning {{assignment through this expression is not modeled}}
  }
  return *w.q;
}

// Control: a direct (non-anonymous) member is the case the merge already handled.
struct [[gsl::Pointer]] D {
  int *p;
  D() : p(nullptr) {}
};
int direct_member(D d [[clang::noescape]]) {
  {
    int local = 0;
    d.p = &local; // expected-warning {{local variable 'local' does not live long enough}}
  } // expected-note {{destroyed here}}
  return *d.p; // expected-note {{later used here}}
}
