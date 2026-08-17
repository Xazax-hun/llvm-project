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
  int i = static_cast<int>(d); // no-warning: not a pointer conversion at all
  // Recovering a typed pointer out of a `void *` IS reported, for the same reason a
  // reinterpret_cast is: `void *` is opaque, so the conversion can name any type and
  // nothing records where the pointer came from. Splitting a conversion in two
  // through one is how a cast evades any check that compares the source and target
  // types -- `Base * -> void * -> Derived *` has a record on only one side of each
  // half, so neither half looks like a base-to-derived conversion.
  int *p = static_cast<int *>(vp); // expected-warning {{recovering a typed pointer from a 'void *' is not modeled by lifetime safety analysis}}
  return i + (p != nullptr);
}

// Casting TO `void *` is not reported: that is the opaque-userdata idiom, and what a
// callee may do with such a parameter is handled conservatively at the call (a
// `void` pointee is assumed to reach an owner).
void *to_void(int *p) { return static_cast<void *>(p); } // no-warning

// Per-construct opt-out.
void opt_out(U u) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-type-punning"
  long v = u.n; // no-warning
  (void)v;
#pragma clang diagnostic pop
}
