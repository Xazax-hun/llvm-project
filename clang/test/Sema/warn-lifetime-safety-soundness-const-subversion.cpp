// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-const-subversion -verify %s

// The analysis assumes a const member function does not invalidate borrows into
// the object. A 'mutable' field or a 'const_cast' can subvert that assumption
// (a const method could reallocate/free state through them), so the safe
// programming model rejects both. Without these checks, a const method that
// invalidates via 'mutable'/'const_cast' is a silent false negative.

struct WithMutable {
  mutable int *data; // expected-warning {{'mutable' field 'data' can be modified by a const member function; lifetime safety assumes const member functions do not invalidate borrows into the object}}
  int normal;
};

struct WithConstCast {
  int *data;
  void reset() const {
    const_cast<WithConstCast *>(this)->data = nullptr; // expected-warning {{'const_cast' can subvert the const-correctness that lifetime safety relies on to assume const member functions do not invalidate borrows into the object}}
  }
};

const int global = 0;
int *strip_global() {
  return const_cast<int *>(&global); // expected-warning {{'const_cast' can subvert the const-correctness}}
}

// No 'mutable' / no 'const_cast' -> clean.
struct Clean {
  int *data;
  int *get() const { return data; }
};

// Per-construct opt-out.
struct OptOut {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-const-subversion"
  mutable int *cache; // no-warning
#pragma clang diagnostic pop
};

//===----------------------------------------------------------------------===//
// A const member function can also subvert const by mutating an owner through
// the pointee of an owning smart-pointer member: 'const' does not propagate
// through a smart pointer, so the dereference yields mutable access.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner(char)]] Buf {
  void mutate();    // non-const mutator
  int read() const; // const accessor
};

void take_mut(Buf &);
void take_const(const Buf &);
void take_val(Buf);

// An owning smart pointer: const operator*/operator-> handing out a non-const
// pointee, exactly like std::unique_ptr.
template <class T> struct [[gsl::Owner]] Ptr {
  T &operator*() const;
  T *operator->() const;
};

struct Doc {
  Ptr<Buf> buf;

  // Mutating uses through the smart pointer in a const method: subversion.
  void via_method() const {
    buf->mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  void via_func_ref() const {
    take_mut(*buf); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  void via_binding() const {
    Buf &r = *buf; // expected-warning {{const member function mutates an owner through a pointer member}}
    r.mutate();
  }

  // Read-only (const) uses are fine.
  void read_method() const { (void)buf->read(); }                    // no-warning
  void read_func() const { take_const(*buf); }                       // no-warning
  void read_binding() const { const Buf &r = *buf; (void)r.read(); } // no-warning
  void copy_out() const { take_val(*buf); }                          // no-warning

  // A non-const method already has mutable access; nothing is subverted.
  void non_const() { buf->mutate(); } // no-warning
};

// The pimpl idiom is not flagged: the pointee is a plain class, not an owner, so
// mutating it cannot dangle a view.
struct Impl {
  void doStuff(); // non-const
};
struct Widget {
  Ptr<Impl> pimpl;
  void run() const { pimpl->doStuff(); } // no-warning
};

// But if the pimpl's pointee *transitively contains* a mutable owner, a const
// method reaching it through the smart pointer gets mutable access to that
// owner -- exactly the subversion: a sibling const accessor can hand out a
// borrow into the owner that this const method then invalidates.
struct OwnerImpl {
  Buf b; // a gsl::Owner field
};
struct OwnerFacade {
  Ptr<OwnerImpl> pimpl;
  // Reaches the owner through the smart pointer in a const method (the field
  // access on the dereferenced pointee is not const-consumed).
  void grow() const {
    pimpl->b.mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
};

// The smart-pointer member need not be a DIRECT `this->member`: a const method
// reaching an owning smart pointer through a chain of value-subobject members
// (`this->a.p`, `this->a.b.p`) still gets mutable access to the pointee, because
// `const` does not propagate through the smart pointer. Detection follows the
// value-subobject chain from `this`.
struct NestedInner {
  Ptr<Buf> p; // owning smart pointer one sub-object deep
};
struct NestedFacade {
  NestedInner in;
  void grow() const {
    in.p->mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  void grow_deref() const {
    (*in.p).mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  // Read-only access through the chain is fine.
  int read() const { return in.p->read(); } // no-warning
};

struct DeepA {
  NestedInner mid;
};
struct DeepFacade {
  DeepA a;
  void grow() const {
    a.mid.p->mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
};

// The const-drop need not be a `*`/`->` on a member: any `this`-derived
// expression with an indirection type whose pointee is non-const subverts
// `const`. A sibling const ACCESSOR that hands out a non-const pointer/reference
// into an owner reached from `this` (e.g. `unique_ptr::get()` re-exposed as a
// `T*`, or a `T&` accessor) is such a crossing -- using it to mutate is flagged.
struct AccessorFacade {
  Ptr<Buf> owned;
  // The accessor bodies themselves hand out mutable access to the owner from a
  // const method (case 1: a non-const-consumed smart-pointer deref).
  Buf *getPtr() const { return owned.operator->(); } // expected-warning {{const member function mutates an owner through a pointer member}}
  Buf &getRef() const { return *owned; }             // expected-warning {{const member function mutates an owner through a pointer member}}

  void via_ptr_arrow() const {
    getPtr()->mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  void via_ptr_deref() const {
    (*getPtr()).mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  void via_ref() const {
    getRef().mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  // Read-only use of the accessors is fine.
  int read_ptr() const { return getPtr()->read(); } // no-warning
  int read_ref() const { return getRef().read(); }  // no-warning
};

// A const accessor returning a CONST pointer/reference is not a crossing.
struct ConstAccessorFacade {
  Ptr<Buf> owned;
  const Buf *getPtr() const { return owned.operator->(); } // no-warning
  const Buf &getRef() const { return *owned; }             // no-warning
  int read_ptr() const { return getPtr()->read(); } // no-warning
  int read_ref() const { return getRef().read(); }  // no-warning
};


//===----------------------------------------------------------------------===//
// `const` does not propagate through a RAW pointer either: a const method that
// mutates an owner through a raw-pointer member is the same subversion.
//===----------------------------------------------------------------------===//

struct RawDoc {
  Buf *buf;

  void arrow_mut() const {
    buf->mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  void deref_func() const {
    take_mut(*buf); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  void deref_binding() const {
    Buf &r = *buf; // expected-warning {{const member function mutates an owner through a pointer member}}
    r.mutate();
  }
  void paren_deref() const {
    (*buf).mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }

  // Read-only / copy uses are fine.
  void read_method() const { (void)buf->read(); }                    // no-warning
  void read_func() const { take_const(*buf); }                       // no-warning
  void read_binding() const { const Buf &r = *buf; (void)r.read(); } // no-warning
  void copy_out() const { take_val(*buf); }                          // no-warning

  // A non-const method already has mutable access; nothing is subverted.
  void non_const() { buf->mutate(); } // no-warning
};

// Pimpl with a raw pointer to a plain (non-owner) class: not flagged.
struct RawWidget {
  Impl *pimpl;
  void run() const { pimpl->doStuff(); } // no-warning
};

//===----------------------------------------------------------------------===//
// `const` does not propagate through the indirection even when the pointer /
// reference member points at a NON-owner record that merely *contains* a mutable
// owner: a const method reaching the owner through it (`this->p->ownerField.
// mutate()`) still gets mutable access and can invalidate a borrow a sibling
// accessor handed out. The member need not point directly at the owner.
//===----------------------------------------------------------------------===//

struct Holder {
  Buf b; // a gsl::Owner field, reached through the indirection below
};

struct ThroughWrapper {
  Holder *hp;
  Holder &hr;
  ThroughWrapper(Holder &h) : hp(&h), hr(h) {}

  // Pointer member to a non-owner that contains the owner.
  void via_ptr() const {
    hp->b.mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  // Reference member to a non-owner that contains the owner.
  void via_ref() const {
    hr.b.mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
  // Read-only access through the indirection is fine.
  void read_ptr() const { (void)hp->b.read(); } // no-warning
  void read_ref() const { (void)hr.b.read(); }  // no-warning
};

// A view whose pointer member reaches an owner through a non-owner intermediate,
// mutating it in a const method -- the bug that motivated this: a borrow into the
// owner co-held by the caller is silently invalidated.
struct [[gsl::Pointer]] WrapperView {
  Holder *p;
  void grow() const {
    p->b.mutate(); // expected-warning {{const member function mutates an owner through a pointer member}}
  }
};

//===----------------------------------------------------------------------===//
// `const` dropped by an explicit C-style / functional cast (not the const_cast
// keyword, which is flagged syntactically): a const method mutating a directly-
// owned owner field through such a cast subverts const just as a `mutable` or a
// pointer indirection would.
//===----------------------------------------------------------------------===//

struct CastDoc {
  Buf b;

  // C-style cast dropping const on the owner field itself. Flagged both at the
  // cast site (a cast that casts away constness) and by the analysis (the const
  // method mutates the owner through the cast).
  void grow_field_cast() const {
    ((Buf &)b).mutate(); // expected-warning {{const member function mutates an owner through a pointer member}} \
                         // expected-warning {{a cast that casts away constness}}
  }
  // C-style cast of `this` to a non-const pointer, then a field mutation.
  void grow_this_cast() const {
    ((CastDoc *)this)->b.mutate(); // expected-warning {{const member function mutates an owner through a pointer member}} \
                                   // expected-warning {{a cast that casts away constness}}
  }
  // Functional-notation cast of `this`.
  void grow_this_func_cast() const {
    ((CastDoc *)(this))->b.mutate(); // expected-warning {{const member function mutates an owner through a pointer member}} \
                                     // expected-warning {{a cast that casts away constness}}
  }

  // A value-preserving cast that does NOT drop const, used read-only: fine.
  int read_const_cast() const { return ((const Buf &)b).read(); } // no-warning
};

//===----------------------------------------------------------------------===//
// A C-style or functional cast that casts away constness is, like the
// 'const_cast' keyword, flagged at the CAST SITE -- independent of how the
// result is later used (a local pointer/reference, a helper, ...). This catches
// const-subversion laundered past the receiver-based analysis (e.g. binding the
// cast to a local pointer first), and a const-dropping void* roundtrip that the
// keyword check does not see.
//===----------------------------------------------------------------------===//

void cstyle_drop_ref(const int &cr) {
  (int &)cr = 5; // expected-warning {{a cast that casts away constness}}
}
void cstyle_drop_ptr(const int *cp) {
  int *p = (int *)cp; // expected-warning {{a cast that casts away constness}}
  *p = 5;
}
void cstyle_drop_void_roundtrip(const int *cp) {
  int *p = (int *)(const void *)cp; // expected-warning {{a cast that casts away constness}}
  *p = 5;
}
using IntPtr = int *;
void functional_drop(const int *cp) {
  IntPtr p = IntPtr(cp); // expected-warning {{a cast that casts away constness}}
  *p = 5;
}

// Negatives: casts that do not remove const.
int value_cast(double d) { return (int)d; }               // no-warning
const int *add_const(int *p) { return (const int *)p; }   // no-warning
char *reinterpret_same_const(int *p) { return (char *)p; } // no-warning
long ptr_to_int(int *p) { return (long)p; }                // no-warning


