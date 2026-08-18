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

// The SPELLING is not what matters: `(T *)p` produces the identical `BitCast` that
// `reinterpret_cast<T *>(p)` does, so testing for the keyword caught only one of them.
// A C-style cast reinterpreting one type's bytes as another walked straight through --
// on a global array, where no other net applies, that was a clean use-after-free.
struct Wide {
  long a, b;
};
alignas(Wide) char g_buf[sizeof(Wide)];

long c_style_pun(char *p) {
  return ((Wide *)p)->a; // expected-warning {{'reinterpret_cast' is not modeled by lifetime safety analysis}}
}

long c_style_pun_array() {
  return ((Wide *)g_buf)->a; // expected-warning {{'reinterpret_cast' is not modeled by lifetime safety analysis}}
}

long functional_pun(char *p) {
  using WideP = Wide *;
  return WideP(p)->a; // expected-warning {{'reinterpret_cast' is not modeled by lifetime safety analysis}}
}

// A reference reinterpretation uses a different cast kind (LValueBitCast) and is
// covered too.
long ref_pun(char &c) {
  return reinterpret_cast<Wide &>(c).a; // expected-warning {{'reinterpret_cast' is not modeled by lifetime safety analysis}}
}

// A non-union struct member access, and a cast that does not reinterpret bytes, stay
// silent. (A C-style cast that DOES reinterpret is reported above -- what counts is the
// cast's kind, not how it is written.)
struct Plain {
  int a;
  int b;
};
int plain_member(Plain &s) { return s.a + s.b; } // no-warning

// Casting TO `void *` is the opaque-userdata idiom and is allowed; a derived-to-base
// conversion and a const conversion are not reinterpretation either.
struct PunBase {
  int x;
};
struct PunDerived : PunBase {
  int y;
};
void *to_void(Plain *p) { return (void *)p; }              // no-warning
PunBase *upcast(PunDerived *p) { return (PunBase *)p; }    // no-warning
const Plain *add_const(Plain *p) { return (const Plain *)p; } // no-warning

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
