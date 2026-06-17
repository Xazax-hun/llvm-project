// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// The assumed-invalidation check must use the MOST-DERIVED receiver type, not
// the static type at the call site. When a derived object's owner field is
// mutated through a method inherited from (or dispatched on) a base class, the
// implicit object argument carries a derived-to-base cast, so its static type
// is the base -- which does not declare the owner field. Testing
// recordHasGslOwnerField on that base type skipped the invalidation, leaving a
// borrow into the derived's owner field tracked as valid across the mutation:
// a silent use-after-free. The receiver type is now recovered by stripping the
// implicit casts.

struct [[gsl::Owner(int)]] MyBuf {
  const int *data() const [[clang::lifetimebound]];
  void grow(); // non-const mutator: may reallocate
};

struct Base {
  virtual void doMutate() = 0;
  void viaBase() { doMutate(); } // receiver static type Base: no owner field
  virtual ~Base() = default;
};

struct Widget : Base {
  MyBuf buf;
  void doMutate() override { buf.grow(); }

  int bug() {
    const int *p = buf.data(); // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    viaBase();                 // expected-note {{assumed to be invalidated by this operation}}
    return *p;
  }
};

// A non-virtual base method reaching the derived owner field through the same
// implicit-object cast is equally covered.
struct NVBase {
  void bump(); // non-const, declared in the base
};
struct Holder : NVBase {
  MyBuf buf;
  int bug2() {
    const int *p = buf.data(); // expected-warning {{object whose reference is captured may be invalidated}}
    bump();                    // expected-note {{assumed to be invalidated by this operation}}
    return *p;
  }
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A const base method cannot mutate the owner -- no warning (the escape hatch
// works through inheritance, just as for a direct const method).
struct ConstBase {
  void look() const;
  virtual ~ConstBase() = default;
};
struct CWidget : ConstBase {
  MyBuf buf;
  int ok() {
    const int *p = buf.data();
    look(); // no-warning: const
    return *p;
  }
};

// A derived with NO owner field: a base method call invalidates nothing.
struct PlainDerived : Base {
  int n;
  void doMutate() override { n++; }
  int ok2() {
    int local = 0;
    const int *p = &local;
    viaBase(); // no-warning: nothing the receiver holds is an owner
    return *p;
  }
};

//===----------------------------------------------------------------------===//
// Virtual dispatch through a base reference/pointer.
//
// When a non-const virtual method is called through a base reference whose
// static type declares no owner field, the dynamic type may be a derived class
// that overrides the method to mutate an owner field -- and IgnoreImpCasts can
// only recover the base static type (the reference's declared type), not the
// runtime type. The open world of derived classes cannot be enumerated
// intra-procedurally, so a non-const virtual call on a polymorphic receiver is
// conservatively assumed to mutate an owner. It only warns when a borrow into
// the receiver is live across the call.
//===----------------------------------------------------------------------===//

struct Iface {
  virtual const int *view() const [[clang::lifetimebound]] = 0;
  virtual void mutate() = 0;       // non-const virtual: may reallocate in an override
  virtual void inspect() const = 0; // const virtual: cannot mutate
  virtual ~Iface() = default;
};
struct Impl : Iface {
  MyBuf buf;
  const int *view() const [[clang::lifetimebound]] override { return buf.data(); }
  void mutate() override { buf.grow(); }
  void inspect() const override {}
};

int virtual_dispatch() {
  Impl impl;
  Iface &i = impl;         // expected-warning {{object whose reference is captured may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  const int *p = i.view();
  i.mutate();              // expected-note {{assumed to be invalidated by this operation}}
  return *p;
}

// Negative: a const virtual call between the borrow and the use cannot mutate.
int virtual_const_ok() {
  Impl impl;
  Iface &i = impl;
  const int *p = i.view();
  i.inspect(); // no-warning: const
  return *p;
}

