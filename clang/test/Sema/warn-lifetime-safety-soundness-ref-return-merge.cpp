// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

// A reference-returning call (`T& f()`) is a glvalue: the AST strips the
// reference, so at the call expression `getType()` is the pointee type and
// `isReferenceType()` is false. When such a call is not a recognized borrow
// accessor and no loan flows into its result, the borrow taken by `&f()` is
// untracked. It must be seeded with the Unknown-loan sentinel (which survives
// dataflow union joins) rather than left as an *empty* origin -- otherwise a
// control-flow merge supplying a valid loan on another path masks the loss and
// the dangle escapes clean (checkLostLoan only fires while the set is empty).

struct Owner {
  int storage = 0;
  // Unrecognized reference-returning accessor (not lifetimebound, not a modeled
  // STL accessor): the returned borrow is untracked.
  int &grow();
};

int global;

// Baseline: taking the address of the untracked reference borrow, with no merge,
// trips lost-loan at the use (the origin is empty there).
void no_merge() {
  Owner o;
  int *p = &o.grow();
  *p = 1; // expected-warning {{cannot track}}
}

// Masked form: the borrow is one arm of a `?:` whose other arm holds a valid
// persistent loan (`&global`). The union merge keeps that valid loan, so an
// *empty* origin would look non-empty and suppress lost-loan. The Unknown
// sentinel keeps it detectable.
void masked(bool c) {
  Owner o;
  int *p = &global;
  p = c ? p : &o.grow(); // expected-warning {{cannot track}}
  *p = 1;                // expected-warning {{cannot track}}
}
