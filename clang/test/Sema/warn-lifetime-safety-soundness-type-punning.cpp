// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-type-punning -verify %s
//
// (-Wlifetime-safety-type-punning is part of the -Wlifetime-safety-soundness
// umbrella; the union corpus entry exercises it under the full umbrella.)

// Under the safe programming model, union member access and 'reinterpret_cast'
// are rejected: both reinterpret the same storage as a different type, which the
// analysis cannot reason about. Union members alias each other (a borrow into
// one member can be invalidated by writing another, which the field-identity-
// keyed invalidation does not see); a 'reinterpret_cast' can launder a borrow
// through an unrelated type, hiding its provenance.

union U {
  int *p;
  long n;
};

void union_read(U u) {
  long v = u.n; // expected-warning {{union member access is not modeled by lifetime safety analysis}}
  (void)v;
}

void union_write(U &u, int *q) {
  u.p = q; // expected-warning {{union member access is not modeled by lifetime safety analysis}}
}

struct HasUnion {
  union {
    int i;
    float f;
  };
};

void anonymous_union(HasUnion &h) {
  h.i = 3; // expected-warning {{union member access is not modeled by lifetime safety analysis}}
}

int reinterpret_round_trip(unsigned long n) {
  int *p = reinterpret_cast<int *>(n); // expected-warning {{'reinterpret_cast' is not modeled by lifetime safety analysis}}
  return *p;
}

long ptr_to_int(int *p) {
  return reinterpret_cast<long>(p); // expected-warning {{'reinterpret_cast' is not modeled by lifetime safety analysis}}
}

// A non-union struct member access and a C-style/static cast stay silent.
struct Plain {
  int a;
  int b;
};
int plain_member(Plain &s) { return s.a + s.b; } // no-warning

int static_casts(double d, void *vp) {
  int i = static_cast<int>(d);     // no-warning
  int *p = static_cast<int *>(vp); // no-warning
  return i + (p != nullptr);
}

// Per-construct opt-out.
void opt_out(U u) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-type-punning"
  long v = u.n; // no-warning
  (void)v;
#pragma clang diagnostic pop
}
