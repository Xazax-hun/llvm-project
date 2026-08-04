// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

volatile char sink;

// Reentrancy through a base-typed parameter. Assumed-invalidation asks whether an
// owner is reachable from a mutated parameter, using the parameter's STATIC type.
// Upcasting the argument to an abstract interface erased that edge: `Reloader` has
// no data members at all, so nothing looked mutable -- while the virtual call in
// the callee dispatches straight back to the derived object and reallocates what it
// owns. The `[[clang::noescape]]` here is truthful, so no body verifier applies,
// and no annotation can express "this virtual call may invalidate anything reachable
// from the argument's complete object". A polymorphic pointee is therefore treated
// conservatively, confirmed against the loans the argument actually carries.

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

// A non-polymorphic base with no owner is unaffected: there is no dynamic type
// that could add one.
struct PlainBase {
  int Tag = 0;
};
void tweak(PlainBase &B [[clang::noescape]]);

struct PlainApp : PlainBase {
  string Cfg;
  void go() {
    string_view V = Cfg;
    tweak(*this); // no-warning: PlainBase is not polymorphic
    sink = *V.data();
  }
};
