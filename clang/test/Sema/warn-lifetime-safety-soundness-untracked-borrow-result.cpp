// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-lost-loan -verify %s

// A call that returns a borrow-carrying value (a view/pointer/reference) but is
// not [[clang::lifetimebound]] and not a recognized accessor produces an
// *untracked* borrow -- the analysis cannot tell what it points into. The result
// is marked with an "unknown" loan so the lost borrow is reported when used, and
// -- crucially -- that marker survives dataflow joins, so a co-resident valid
// borrow on another path does not mask the loss.

struct [[gsl::Owner]] Owner {
  ~Owner();
};
struct [[gsl::Pointer]] View {
  View();
  View(const Owner &o [[clang::lifetimebound]]); // establishes a real borrow
  View slice() const; // NOT lifetimebound: result is an untracked borrow
  int size() const;
};
View pick(); // free function returning an untracked borrow

void untracked_result_used() {
  Owner o;
  View good = o;          // borrow into o (propagated)
  View b = good.slice();  // slice() drops the borrow -> untracked
  (void)b;                // expected-warning {{no borrow information flows into it}}
  (void)good;             // no-warning (good still holds the real borrow)
}

// Masking: `v` holds a valid borrow on one path and an untracked borrow on
// another; the loss must still be reported (the join does not hide it).
void masked_loss(bool c) {
  Owner o;
  View v = o;   // valid borrow into o
  if (c)
    v = pick(); // untracked borrow on this path
  (void)v;      // expected-warning {{no borrow information flows into it}}
}

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A lifetimebound constructor propagates the borrow; not untracked.
void lifetimebound_ok() {
  Owner o;
  View v = o; // no-warning (borrow propagated)
  (void)v;
}

// A non-borrow return type is unaffected.
int value_ok(View a [[clang::noescape]]) {
  return a.size(); // no-warning (returns a value, not a borrow)
}
