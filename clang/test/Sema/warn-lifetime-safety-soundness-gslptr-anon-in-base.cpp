// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A view member can be reached through BOTH an anonymous struct AND a
// gsl::Pointer base class at once: the member is declared in an anonymous struct
// nested in a [[gsl::Pointer]] base of a non-gsl derived receiver. Earlier fixes
// recognized each shape alone -- the anonymous-record base peel (member of an
// anon struct of a gsl::Pointer receiver) and the gsl::Pointer-base declaring
// class (member directly in a gsl::Pointer base) -- but the combination landed
// on a member whose declaring class is the anonymous struct (not a gsl::Pointer)
// reached through a non-gsl derived, so neither branch fired and the store was
// dropped. The merge now walks the member's declaring record outward through
// enclosing anonymous records to find the enclosing gsl::Pointer.

struct [[gsl::Pointer]] Base {
  struct {
    int *p;
  }; // anonymous struct inside the gsl::Pointer base
};
struct D : Base {
  int extra;
};

int sink;
void anon_in_base(D &d [[clang::noescape]]) {
  {
    int local = 7;
    d.p = &local; // expected-warning {{assignment through this expression is not modeled}}
  }
  sink = *d.p;
}

// Through a pointer-to-derived.
void anon_in_base_via_ptr(D *d [[clang::noescape]]) {
  {
    int local = 7;
    d->p = &local; // expected-warning {{local variable 'local' does not live long enough}}
  } // expected-note {{destroyed here}}
  sink = *d->p; // expected-note {{later used here}}
}
