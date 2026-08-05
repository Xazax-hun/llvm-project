// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

volatile char sink;

// Reentrancy through a base-typed parameter. Assumed-invalidation asks whether an
// owner is reachable from a mutated parameter. Taking that from the parameter's
// STATIC type erased the edge whenever the argument was upcast: `Reloader` has no
// data members at all, so nothing looked mutable -- while the callee reaches the
// derived object again and reallocates what it owns. The `[[clang::noescape]]` here
// is truthful, so no body verifier applies, and no annotation can express "this
// call may invalidate anything reachable from the argument's complete object".
//
// The two questions are therefore split. MUTABILITY comes from the parameter: can
// the callee write through it at all (a non-const pointer/reference)? REACHABILITY
// comes from neither static type -- not the parameter's, which may be the base, and
// not the argument's, since the upcast may have happened earlier. It is confirmed
// from the loans the argument actually carries (OwnerLoanGate::DenotedOwner): a loan
// must denote an object that is-a the parameter's type and is (or contains) a mutable
// owner. An argument denoting no owner therefore yields nothing.

struct Reloader {
  virtual ~Reloader() = default;
  virtual void reload() = 0;
};

void notifyAll(Reloader &R [[clang::noescape]]) { R.reload(); }

struct App : Reloader {
  string Cfg;
  void reload() override { Cfg = string(); } // may reallocate Cfg
  void go() {
    string_view V = Cfg; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    notifyAll(*this);    // expected-note {{assumed to be invalidated by this operation}}
    sink = *V.data();
  }
};

// A pointer parameter, and a base that is not abstract.
struct Handler {
  virtual ~Handler() = default;
  virtual void run() {}
};
void dispatch(Handler *H [[clang::noescape]]) { H->run(); }

struct Service : Handler {
  vector<int> Data;
  void run() override { Data.push_back(1); }
  void go() {
    int *P = &Data[0]; // expected-warning {{may be invalidated by an operation}}
    dispatch(this);    // expected-note {{assumed to be invalidated by this operation}}
    sink = *P ? 1 : 0;
  }
};

//===----------------------------------------------------------------------===//
// Negatives: the loan-based confirmation keeps these clean.
//===----------------------------------------------------------------------===//

// The argument is polymorphic but denotes an object that owns nothing, so it
// cannot reallocate anything -- no loan of it points at a mutable owner.
struct Visitor {
  virtual ~Visitor() = default;
  virtual void visit() = 0;
};
struct Counter : Visitor {
  int N = 0;
  void visit() override { ++N; }
};
void runVisitor(Visitor &V [[clang::noescape]]);

struct OwnerApp {
  string Cfg;
  void go() {
    string_view V = Cfg;
    Counter C;
    runVisitor(C); // no-warning: 'C' owns nothing reallocatable
    sink = *V.data();
  }
};

// The argument is a disjoint sibling FIELD that happens to be polymorphic and
// owner-containing. A member access inherits only its enclosing object's loan, so
// accepting the enclosing-object fallback here would flag borrows of siblings --
// mutating `B` cannot reach `Cfg`.
struct Sink2 {
  virtual ~Sink2() = default;
  virtual void put() = 0;
};
struct Buf : Sink2 {
  vector<int> D;
  void put() override { D.push_back(1); }
};
void drive(Sink2 &S [[clang::noescape]]);

struct TwoFields {
  string Cfg;
  Buf B;
  void go() {
    string_view V = Cfg;
    drive(B); // no-warning: mutating 'B' cannot reach the sibling 'Cfg'
    sink = *V.data();
  }
};

// A const reference cannot mutate, so no invalidation is assumed.
struct Reader {
  virtual ~Reader() = default;
  virtual void read() const = 0;
};
void look(const Reader &R [[clang::noescape]]);

struct ConstApp : Reader {
  string Cfg;
  void read() const override {}
  void go() {
    string_view V = Cfg;
    look(*this); // no-warning: const reference
    sink = *V.data();
  }
};

// A NON-polymorphic base is reported too. Virtual dispatch is not the only route
// back down to the derived object: the callee can reach it with a plain
// `static_cast<PlainApp &>(B)`, no vtable involved. Gating on "the parameter's
// pointee is polymorphic" was therefore the wrong criterion; the loan denotes
// `PlainApp`, which owns `Cfg`, and that is what confirms the hazard.
struct PlainBase {
  int Tag = 0;
};
void tweak(PlainBase &B [[clang::noescape]]);

struct PlainApp : PlainBase {
  string Cfg;
  void go() {
    string_view V = Cfg; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    tweak(*this);        // expected-note {{assumed to be invalidated by this operation}}
    sink = *V.data();
  }
};

// CRTP / static polymorphism is the same shape: `CrtpBase<CrtpApp>` is not
// polymorphic, yet `static_cast<D *>(this)->doReload()` dispatches back down.
template <class D> struct CrtpBase {
  void reload() { static_cast<D *>(this)->doReload(); }
};
template <class D> void notifyCrtp(CrtpBase<D> &B [[clang::noescape]]);

struct CrtpApp : CrtpBase<CrtpApp> {
  string Cfg;
  void doReload() { Cfg = string(); }
  void go() {
    string_view V = Cfg;  // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    notifyCrtp(*this);    // expected-note {{assumed to be invalidated by this operation}}
    sink = *V.data();
  }
};

// Negative: a base with no owner anywhere reachable from the object the loan
// denotes still yields nothing -- reachability is a property of the loan, not of any
// declared type.
struct OwnerlessApp : PlainBase {
  int n = 0;
  void go() {
    tweak(*this); // no-warning: nothing reachable from OwnerlessApp is an owner
  }
};

// The upcast may also happen BEFORE the call, so the argument's static type is the
// base too. This is why no static type can answer reachability and the loan has to:
// the gate asks the parameter only whether it can be written through.
struct PreUpcastApp : PlainBase {
  string Cfg;
  void go() {
    string_view V = Cfg; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    PlainBase &B = *this;
    tweak(B); // expected-note {{assumed to be invalidated by this operation}}
    sink = *V.data();
  }
};
