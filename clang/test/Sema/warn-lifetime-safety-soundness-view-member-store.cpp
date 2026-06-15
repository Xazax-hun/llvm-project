// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A store into a borrow-holding member of a [[gsl::Pointer]] view object
// (`v.p = local`) was silently dropped: a gsl::Pointer is a leaf in the origin
// tree (its members are not tracked per field), so the store landed on a
// transient member-access origin disconnected from `v`, while `v` kept whatever
// borrow it already held -- masking the loss. The store now MERGES the stored
// value's loans into the view's own origin (without killing), so the view
// reflects the borrow and a use of it after the borrow's source expires is
// caught.

struct [[gsl::Pointer]] V {
  const char *p;
  unsigned n;
};

void use(const char *q [[clang::noescape]]);

// Storing a borrow of a local into the view member, then using the view after
// the local dies.
void store_local(V v [[clang::noescape]]) {
  {
    char local[64];
    v.p = local; // expected-warning {{local variable 'local' does not live long enough}}
  }              // expected-note {{destroyed here}}
  use(v.p);      // expected-note {{later used here}}
}

// Negative: storing a long-lived borrow (a static buffer) into the view member
// stays silent.
void store_static(V v [[clang::noescape]]) {
  static char buf[64];
  v.p = buf;
  use(v.p); // no-warning
}

// A NESTED store into a view member reached through another view member
// (`v.inner.p = local`). Both the outer object and its `inner` member are
// leaves in the origin tree, so the immediate base (`v.inner`) is a transient
// member-access origin disconnected from `v`. The merge must reach the
// OUTERMOST object `v` -- found by climbing the origin-tree parent chain --
// otherwise a later read routed to `v` would not see the borrow.
struct [[gsl::Pointer]] Outer {
  V inner;
  const char *q;
};

void store_nested(Outer v [[clang::noescape]]) {
  {
    char local[64];
    v.inner.p = local; // expected-warning {{local variable 'local' does not live long enough}}
  }                    // expected-note {{destroyed here}}
  use(v.inner.p);      // expected-note {{later used here}}
}

// Negative: a nested store of a long-lived borrow stays silent.
void store_nested_static(Outer v [[clang::noescape]]) {
  static char buf[64];
  v.inner.p = buf;
  use(v.inner.p); // no-warning
}
