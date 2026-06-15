// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// An RAII guard that is a [[gsl::Pointer]] capturing a mutable owner (via a
// lifetime_capture_by(this) / owner-pointer constructor) may mutate or free
// that owner in its out-of-line destructor (`~Guard() { o->grow(); }`), which
// the intra-procedural analysis cannot see. The guard's destruction is treated
// as an assumed invalidation of the borrows it carries on the captured owner,
// so a view into that owner that outlives the guard's scope is reported. A
// plain view/wrapper with a trivial destructor (which cannot run such code) is
// not affected -- only a non-trivial destructor triggers this.

struct [[gsl::Owner(int)]] Owner {
  const int *view() const [[clang::lifetimebound]];
  void grow(); // mutating (reallocating) method
};

struct [[gsl::Pointer]] Guard {
  Owner *o;
  Guard(Owner *oo [[clang::lifetime_capture_by(this)]]);
  ~Guard(); // non-trivial: may mutate *o
};

// A view that outlives the guard's scope: the guard's destruction may free the
// owner, so the later use dangles.
void captured_then_destroyed() {
  Owner owner;
  const int *v = owner.view(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  { // expected-note {{assumed to be invalidated by this operation}}
    Guard g(&owner);
  }
  (void)*v;
}

// Precision: a view used only DURING the guard's lifetime (before its
// destructor runs) is still valid -- no warning.
void used_during_guard_life() {
  Owner owner;
  Guard g(&owner);
  const int *v = owner.view();
  (void)*v; // no-warning -- g not yet destroyed
}

// A trivial-destructor view/wrapper that merely borrows the same mutable owner
// is not flagged at its destruction (it runs no code).
struct [[gsl::Pointer]] PlainView {
  Owner *o;
  PlainView(Owner *oo);
  // trivial (implicit) destructor
};
void trivial_dtor_wrapper_silent() {
  Owner owner;
  const int *v = owner.view();
  {
    PlainView pv(&owner);
  }
  (void)*v; // no-warning
}
