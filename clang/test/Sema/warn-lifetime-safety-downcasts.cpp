// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-downcasts -verify %s

volatile char sink;

// Inside a constructor or destructor of a class C, the derived part of the object
// either has not been constructed yet or has already been destroyed, so referring
// to a member or base class outside C's own subobject is undefined
// ([class.cdtor]). By the time a base destructor runs, every heap buffer a derived
// member owned has been freed -- so a base destructor that reads derived state is
// a use-after-free, and nothing at the read distinguishes it from a live object.
//
// Chasing where the conversion happens does not work: it may be several calls
// away, applied to a parameter or a pointer read from the heap, in a body that
// lives in another translation unit. So the hazard is DECLARED. A function that
// performs a base-to-derived conversion, or calls a function marked
// '[[clang::downcasts]]', must be marked itself; a constructor or destructor may
// not perform one nor call a function marked that way.

struct Base {
  virtual ~Base();
  // expected-note@+1 {{overridden virtual function is here}}
  virtual void tick();
};
struct Derived : Base {
  int x;
};

//===----------------------------------------------------------------------===//
// The marker is required wherever a conversion happens, and propagates to callers.
//===----------------------------------------------------------------------===//

// expected-warning@+1 {{'raw' performs a base-to-derived conversion but is not marked '[[clang::downcasts]]'}}
Derived *raw(Base *b) {
  return static_cast<Derived *>(b); // expected-note {{base-to-derived conversion to 'Derived' is here}}
}

[[clang::downcasts]] Derived *marked(Base *b) { return static_cast<Derived *>(b); }

// A caller of a marked function needs the marker too, or a constructor or
// destructor further up cannot see the hazard.
// expected-warning@+1 {{'forwards' calls 'marked', which is marked '[[clang::downcasts]]', but is not marked that way itself}}
Derived *forwards(Base *b) { return marked(b); }

[[clang::downcasts]] Derived *forwards_ok(Base *b) { return marked(b); }

// The cast kind does not matter, and neither does what is converted: the operand
// may be a parameter, a member, or a pointer read from the heap.
// expected-warning@+1 {{'c_style' performs a base-to-derived conversion}}
Derived *c_style(Base *b) {
  return (Derived *)b; // expected-note {{conversion to 'Derived' is here}}
}
// expected-warning@+1 {{'reinterp' performs a base-to-derived conversion}}
Derived &reinterp(Base &b) {
  return static_cast<Derived &>(b); // expected-note {{conversion to 'Derived' is here}}
}
struct HoldsBase {
  Base *p;
  // expected-warning@+1 {{'get' performs a base-to-derived conversion}}
  Derived *get() {
    return static_cast<Derived *>(p); // expected-note {{conversion to 'Derived' is here}}
  }
};

//===----------------------------------------------------------------------===//
// A constructor or destructor may not reach one. No marker makes it safe, so the
// hazard is reported rather than a missing marker.
//===----------------------------------------------------------------------===//

struct DtorConverts : Base {
  ~DtorConverts() override {
    // expected-warning@+1 {{destructor of 'DtorConverts' performs a base-to-derived conversion to 'Derived'}}
    (void)static_cast<Derived *>((Base *)this);
  }
};

struct DtorCalls : Base {
  ~DtorCalls() override {
    marked(this); // expected-warning {{destructor of 'DtorCalls' calls 'marked', which is marked '[[clang::downcasts]]'}}
  }
};

struct CtorCalls : Base {
  CtorCalls() {
    marked(this); // expected-warning {{constructor of 'CtorCalls' calls 'marked', which is marked '[[clang::downcasts]]'}}
  }
};

// The marker does not record WHOSE object is converted, so a constructor or
// destructor that converts an unrelated object is reported too.
struct OwnsOther {
  Base *other;
  ~OwnsOther() {
    // expected-warning@+1 {{destructor of 'OwnsOther' performs a base-to-derived conversion to 'Derived'}}
    (void)static_cast<Derived *>(other);
  }
};

//===----------------------------------------------------------------------===//
// The CRTP lifecycle hook: the idiomatic way to write the mistake. The conversion
// sits in `self()`, so nothing in the destructor body names a derived type.
//===----------------------------------------------------------------------===//

template <class D> struct Lifecycle {
  // expected-warning@+1 {{'self' performs a base-to-derived conversion but is not marked}}
  D &self() {
    return static_cast<D &>(*this); // expected-note {{conversion to 'Session' is here}}
  }
  ~Lifecycle() { self().on_destroy(); }
};
struct Session : Lifecycle<Session> {
  int id;
  void on_destroy() { sink = (char)id; }
};
void use_session() { Session s; }

// Once `self()` carries the marker, the destructor calling it is what is reported.
template <class D> struct Lifecycle2 {
  [[clang::downcasts]] D &self() { return static_cast<D &>(*this); }
  // expected-warning@+1 {{destructor of 'Lifecycle2<Session2>' calls 'self', which is marked}}
  ~Lifecycle2() { self().on_destroy(); }
};
struct Session2 : Lifecycle2<Session2> {
  int id;
  void on_destroy() { sink = (char)id; }
};
void use_session2() { Session2 s; }

//===----------------------------------------------------------------------===//
// Not reported.
//===----------------------------------------------------------------------===//

// 'dynamic_cast' is exempt, and is the way to write a checked downcast in a
// constructor or destructor: there the object is treated as being of the
// constructor's or destructor's own class, so it is well defined and simply fails.
Derived *checked(Base *b) { return dynamic_cast<Derived *>(b); } // no-warning
struct DtorDynamic : Base {
  ~DtorDynamic() override {
    (void)dynamic_cast<Derived *>((Base *)this); // no-warning
  }
};

// An UPcast is not a downcast, in a destructor or anywhere else.
Base *up(Derived *d) { return static_cast<Base *>(d); } // no-warning
struct DtorUpcast : Derived {
  ~DtorUpcast() { (void)static_cast<Base *>((Derived *)this); } // no-warning
};

// A conversion between unrelated types is not a downcast either.
struct Unrelated {};
Unrelated *punned(Base *b) { return reinterpret_cast<Unrelated *>(b); } // no-warning

// A destructor calling an unmarked function is fine -- that function is where its
// own conversions get reported.
void plain(Base *);
struct DtorPlain : Base {
  ~DtorPlain() override { plain(this); } // no-warning
};

//===----------------------------------------------------------------------===//
// The marker is a HAZARD, not a promise, so the override rule runs the other way
// round from every sibling attribute: the danger is an override that ADDS it,
// since a call through the base is checked against the base's declaration.
//===----------------------------------------------------------------------===//

struct Adder : Base {
  // expected-warning@+1 {{this override is marked '[[clang::downcasts]]' but the function it overrides is not}}
  [[clang::downcasts]] void tick() override { (void)static_cast<Derived *>((Base *)this); }
};

// Dropping it is fine: the override reaches no conversion, so callers through the
// base are not misled.
struct MarkedBase {
  [[clang::downcasts]] virtual void go();
  virtual ~MarkedBase();
};
struct Dropper : MarkedBase {
  void go() override {} // no-warning
};
