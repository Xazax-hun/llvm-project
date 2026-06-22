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


