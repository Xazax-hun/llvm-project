// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// Reading a borrow-holding member of a [[gsl::Pointer]] view (`out = r.p`)
// extracts the borrow the view holds. A gsl::Pointer is a leaf in the origin
// tree (its members are not field-expanded), so the member access otherwise
// lands on a transient origin disconnected from the view, dropping the borrow.
// Normally that empty origin trips lost-loan, but a control-flow merge that
// assigns a valid loan on another path would mask it -- so the read must flow
// the view's held borrow (read-side dual of the view-member store merge).

struct [[gsl::Pointer]] Ref {
  const int *p;
};

int g = 100;

// The bypass: the dropped borrow is masked by the valid `&g` loan merged from
// the other path. Reading r.p must carry &local so the use after scope fires.
int merge_masks(bool cond) {
  const int *out;
  if (cond) {
    out = &g;
  } else {
    int local = 7;
    Ref r{&local}; // expected-warning {{'local' does not live long enough}}
    out = r.p;
  } // expected-note {{destroyed here}}
  return *out; // expected-note {{later used here}}
}

// Negative: a long-lived borrow read out of the view stays silent.
int long_lived(bool cond) {
  const int *out;
  if (cond) {
    out = &g;
  } else {
    Ref r{&g};
    out = r.p;
  }
  return *out; // no-warning
}
