// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-downcasts \
// RUN:   -Wlifetime-safety-type-punning -verify %s

volatile char sink;

// Whether the derived part of an object is alive depends on how far the complete
// object's construction has got and whether its destruction has begun -- and
// nothing at a base-to-derived conversion reveals that. Inside a constructor or
// destructor of a base it is undefined ([class.cdtor]), and since bases are
// destroyed AFTER the derived part, a base destructor that reaches derived state
// reads storage a derived member has already freed.
//
// The analysis cannot catch it: it models `this` as a live complete object, so the
// conversion, the member access and the read all look perfectly modelable and no
// borrow ever expires. So the construct is refused rather than analyzed, like
// 'reinterpret_cast', inline asm, 'setjmp' and exceptions. Refusing it everywhere
// rather than only in a constructor or destructor is what makes the rule complete:
// the conversion may sit in a helper several calls away, in a lambda, in a
// member-initializer list, or in a template instantiation.

struct Base {
  virtual ~Base();
  virtual void tick();
  int tag;
};
struct Derived : Base {
  int x;
};

//===----------------------------------------------------------------------===//
// Reported wherever it appears, and for any spelling.
//===----------------------------------------------------------------------===//

Derived *static_cast_form(Base *b) {
  return static_cast<Derived *>(b); // expected-warning {{a base-to-derived conversion is not modeled by lifetime safety analysis}}
}

Derived *c_style_form(Base *b) {
  return (Derived *)b; // expected-warning {{a base-to-derived conversion is not modeled}}
}

Derived &reference_form(Base &b) {
  return static_cast<Derived &>(b); // expected-warning {{a base-to-derived conversion is not modeled}}
}

// In a destructor -- the shape that is an outright use-after-free.
struct InDtor : Base {
  ~InDtor() override {
    // expected-warning@+1 {{a base-to-derived conversion is not modeled}}
    (void)static_cast<Derived *>((Base *)this);
  }
};

// In a constructor, where the derived part is not constructed yet.
struct InCtor : Base {
  InCtor() {
    // expected-warning@+1 {{a base-to-derived conversion is not modeled}}
    (void)static_cast<Derived *>((Base *)this);
  }
};

// In a member-initializer list, which a walk over the function body would miss.
struct InMemInit {
  char v;
  InMemInit(Base *b)
      // expected-warning@+1 {{a base-to-derived conversion is not modeled}}
      : v((char)static_cast<Derived *>(b)->x) {}
};

// KNOWN GAP: a default member initializer is not reached. The CFG does not descend
// into a CXXDefaultInitExpr's subexpression, so no expression-level check sees it --
// this predates the refusal and is not specific to it.
struct InNSDMI {
  Base *p;
  char v = (char)static_cast<Derived *>(p)->x; // no-warning (see above)
};
void use_nsdmi(Base *b) {
  InNSDMI n{b};
  sink = n.v;
}

// In a lambda, including one at namespace scope.
auto in_namespace_lambda = [](Base *b) {
  return (char)static_cast<Derived *>(b)->x; // expected-warning {{a base-to-derived conversion is not modeled}}
};

void in_local_lambda(Base *b) {
  auto f = [](Base *p) {
    return (char)static_cast<Derived *>(p)->x; // expected-warning {{a base-to-derived conversion is not modeled}}
  };
  sink = f(b);
}

// The CRTP lifecycle hook: the conversion is in `self()`, so nothing in the
// destructor names a derived type. Reported at the conversion, in every
// instantiation-independent spelling.
template <class D> struct Lifecycle {
  D &self() {
    return static_cast<D &>(*this); // expected-warning {{a base-to-derived conversion is not modeled}}
  }
  // expected-note@+1 {{in instantiation of member function 'Lifecycle<Session>::self' requested here}}
  ~Lifecycle() { self().on_destroy(); }
};
// expected-note@+1 {{in instantiation of member function 'Lifecycle<Session>::~Lifecycle' requested here}}
struct Session : Lifecycle<Session> {
  int id;
  void on_destroy() { sink = (char)id; }
};
void use_session() { Session s; }

//===----------------------------------------------------------------------===//
// Not reported.
//===----------------------------------------------------------------------===//

// 'dynamic_cast' is checked at run time, and inside a constructor or destructor
// the object is treated as being of that constructor's or destructor's own class,
// so a conversion to a derived type is well defined and simply fails. It is the
// way to write a checked downcast.
Derived *checked(Base *b) { return dynamic_cast<Derived *>(b); } // no-warning
struct DtorDynamic : Base {
  ~DtorDynamic() override {
    (void)dynamic_cast<Derived *>((Base *)this); // no-warning
  }
};

// An UPcast is not a downcast, explicit or implicit.
Base *up_explicit(Derived *d) { return static_cast<Base *>(d); } // no-warning
Base *up_implicit(Derived *d) { return d; }                     // no-warning
void up_ref(Derived &d) {
  Base &b = d; // no-warning
  (void)b;
}

// A conversion between unrelated types is not a downcast (it is refused on its
// own account when spelled 'reinterpret_cast').
struct Unrelated {};
Unrelated *unrelated(Unrelated *u) { return static_cast<Unrelated *>(u); } // no-warning

// A conversion to the same class is not a downcast.
Derived *same(Derived *d) { return static_cast<Derived *>(d); } // no-warning

//===----------------------------------------------------------------------===//
// A pointer to member, where the hazardous direction is REVERSED.
//
// Converting `Derived::*` to `Base::*` is what lets a Base object reach a member
// only the derived class has (`b->*p`), so that is the dangerous one -- the mirror
// of the object-pointer case. `Base::* -> Derived::*` is the implicit, safe
// widening: a member of Base is a member of every Derived.
//
// This needs its own test because a member-pointer type has no pointee CLASS to
// peel -- peeling yields the pointed-to type (`int`), not the class -- so both
// sides came back null and the conversion was never examined. It is also the only
// laundering channel that produces no laundered pointer VALUE: `this->*p` looks
// like an ordinary member access, so no other net saw it either.
//===----------------------------------------------------------------------===//

int Base::*to_base() {
  // expected-warning@+1 {{a base-to-derived conversion is not modeled}}
  return static_cast<int Base::*>(&Derived::x);
}

struct MemPtrInDtor : Base {
  ~MemPtrInDtor() override {
    // expected-warning@+1 {{a base-to-derived conversion is not modeled}}
    int Base::*p = static_cast<int Base::*>(&Derived::x);
    sink = (char)((Base *)this->*p);
  }
};

// A constant initializer at file scope is analyzed too. It was skipped because a
// member-pointer variable cannot hold a borrow, which is true but decides only
// whether the initializer is looked at -- and this one contains the conversion.
// expected-warning@+1 {{a base-to-derived conversion is not modeled}}
int Base::*const g_field = static_cast<int Base::*>(&Derived::x);

// The safe widening direction is not reported, nor is a same-class conversion.
int widen(Derived *d) {
  int Base::*p = &Base::tag;
  int Derived::*q = p; // no-warning
  return d->*q;
}
int same_class(Base *b) {
  int Base::*p = static_cast<int Base::*>(&Base::tag); // no-warning
  return b->*p;
}

//===----------------------------------------------------------------------===//
// Laundering the conversion through `void *`.
//
// Comparing the source and target types is defeated by splitting the conversion in
// two: `Base * -> void *` has no record on the target side and `void * -> Derived *`
// none on the source side, so neither half looks like a downcast. Recovering a typed
// pointer out of a `void *` is refused in its own right, under
// -Wlifetime-safety-type-punning, on the same grounds as a 'reinterpret_cast': the
// conversion can name any type and nothing records where the pointer came from.
//===----------------------------------------------------------------------===//

struct ViaVoid : Base {
  ~ViaVoid() override {
    void *p = this; // no-warning: casting TO void * is the userdata idiom
    // expected-warning@+1 {{recovering a typed pointer from a 'void *' is not modeled}}
    (void)static_cast<Derived *>(p);
  }
};
