// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A plain (non-gsl) aggregate that holds a borrow but whose ownership the
// analysis cannot track is reported at a local/member declaration (VisitDeclStmt)
// and at a call result. But an aggregate *temporary* that escapes -- returned,
// or whose member is stored to a global -- is covered by neither: its captured
// borrow is orphaned (handleGslAggregateInit only models gsl::Pointer/Owner
// aggregates) and silently dropped. Flag the escaping aggregate temporary as
// unknown-ownership too.

struct Box {
  int *p;
  int n;
};

int *g;

void store_member_to_global() {
  int x = 5;
  g = Box{&x, 0}.p; // expected-warning {{type 'Box' can hold a borrow but is annotated neither}}
}

Box return_aggregate() {
  int x = 5;
  return Box{&x, 0}; // expected-warning {{type 'Box' can hold a borrow but is annotated neither}}
}

struct Inner {
  int *p;
};
struct Outer {
  Inner in;
  int n;
};

Outer return_nested() {
  int x = 5;
  // Both the outer and the (escaping) inner aggregate temporary are untracked.
  return Outer{Inner{&x}, 0}; // expected-warning {{type 'Inner' can hold a borrow but is annotated neither}} \
                              // expected-warning {{type 'Outer' can hold a borrow but is annotated neither}}
}

// Negative: an aggregate temporary that directly initializes a local is reported
// once, at the declaration -- not a second time via the temporary path.
void local_init() {
  int x = 5;
  Box b{&x, 0}; // expected-warning {{type 'Box' can hold a borrow but is annotated neither}}
  (void)b;
}

// Negative: an aggregate of a non-borrow-holding type is not flagged.
int plain_temporary() {
  struct Plain {
    int a;
    int b;
  };
  return Plain{1, 2}.a; // no-warning
}
