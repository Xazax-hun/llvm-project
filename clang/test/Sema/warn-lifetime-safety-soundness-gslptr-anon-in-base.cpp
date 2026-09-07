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
//
// A base subobject's fields now get origins too, so both stores are additionally
// diagnosed precisely. The unmodelled-store refusal still fires here -- reaching
// the member through the anonymous record is a separate step -- so the reports
// stack rather than replace one another.

struct [[gsl::Pointer]] Base {
  struct {
    int *p; // expected-note 2 {{this field dangles}}
  }; // anonymous struct inside the gsl::Pointer base
};
struct D : Base {
  int extra;
};

int sink;
void anon_in_base(D &d [[clang::noescape]]) {
  {
    int local = 7;
    // expected-warning@+3 {{stack memory associated with local variable 'local' escapes to the field 'p'}}
    // expected-warning@+2 {{assignment through this expression is not modeled}}
    // expected-warning@+1 {{local variable 'local' does not live long enough}}
    d.p = &local;
  } // expected-note {{destroyed here}}
  sink = *d.p; // expected-note {{later used here}}
}

// Through a pointer-to-derived.
void anon_in_base_via_ptr(D *d [[clang::noescape]]) {
  {
    int local = 7;
    // expected-warning@+3 {{stack memory associated with local variable 'local' escapes to the field 'p'}}
    // expected-warning@+2 {{assignment through this expression is not modeled}}
    // expected-warning@+1 {{local variable 'local' does not live long enough}}
    d->p = &local;
  } // expected-note {{destroyed here}}
  sink = *d->p; // expected-note {{later used here}}
}
