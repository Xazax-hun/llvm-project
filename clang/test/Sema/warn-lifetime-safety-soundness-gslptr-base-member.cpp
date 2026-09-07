// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A view member can live in a [[gsl::Pointer]] BASE CLASS of a non-gsl
// most-derived type. Storing a borrow into it (`d.p = &local`) is, like a store
// into a direct view member, a store into a gsl::Pointer leaf that must not be
// dropped -- but the view-member-store merge was gated on the receiver's own
// static type being a gsl::Pointer. For `Derived d` (plain) the receiver type is
// not a gsl::Pointer, so the merge was skipped, the borrow dropped, and a later
// dangling read silently missed (when the object enters as a parameter, so the
// unknown-ownership-at-declaration backstop doesn't fire). The merge now also
// recognizes a member declared in a gsl::Pointer base subobject.
//
// A base subobject's fields now get origins of their own, so the store is modeled
// rather than refused: the unmodelled-store report is gone and the dangling read
// is diagnosed precisely instead.

struct [[gsl::Pointer]] ViewBase {
  const int *p; // expected-note {{this field dangles}}
};
struct Derived : ViewBase {
  int extra;
};

int sink;
void base_member_store(Derived &d [[clang::noescape]]) {
  {
    int local = 42;
    // A local's borrow stored into a member of caller-owned storage dangles once
    // we return, and the read below is a use after the local's scope.
    // expected-warning@+2 {{stack memory associated with local variable 'local' escapes to the field 'p' which will dangle}}
    // expected-warning@+1 {{local variable 'local' does not live long enough}}
    d.p = &local;
  } // expected-note {{destroyed here}}
  sink = *d.p; // expected-note {{later used here}}
}
