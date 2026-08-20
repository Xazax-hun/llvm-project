// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

// Two arguments of one call that alias, where the mutating one is upcast to an
// abstract interface. The overlap check asked whether the mutable-reference
// parameter could reach an owner from its STATIC type, and an interface declares
// no data members -- so nothing looked mutable, while a virtual call on it
// dispatches into the derived object and reallocates a std::string there.
// Declaring the parameter with the derived type was reported, so a single upcast
// silenced the same bug.
//
// The parameter type no longer decides it: any non-const pointer/reference to a
// class opens the gate, and the checker confirms the hazard from the loans the
// two arguments carry, so a report needs them to genuinely alias. That mirrors
// the loan-confirmed arm the assumed-invalidation path already used for this
// shape.
//
// "Is the pointee polymorphic" is deliberately NOT the gate. Virtual dispatch is
// only one way back down to the derived object; a plain static_cast reaches it
// with no vtable at all, so a non-polymorphic base erases the owner just as
// effectively (see nonpoly_base_upcast below).

struct IResettable {
  virtual ~IResettable() = default;
  virtual void reset() = 0;
};

struct [[gsl::Owner]] Record : IResettable {
  std::string name;
  std::string_view getName() const [[clang::lifetimebound]] { return name; }
  void reset() override { name.clear(); }
};

// The callee is beyond reproach: both annotations are truthful, and nothing here
// tells it that its two parameters may be the same object.
void resetAndLog(IResettable &target [[clang::noescape]],
                 std::string_view label [[clang::noescape]]);

void via_base() {
  Record r;
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  resetAndLog(r, r.getName()); // expected-note {{assumed to be invalidated by this operation}}
}

// The same call with the parameter spelled as the derived type, which was
// reported before this change and must keep being reported.
void resetAndLogDerived(Record &target [[clang::noescape]],
                        std::string_view label [[clang::noescape]]);

void via_derived() {
  Record r;
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  resetAndLogDerived(r, r.getName()); // expected-note {{assumed to be invalidated by this operation}}
}

//===----------------------------------------------------------------------===//
// A NON-polymorphic base erases the owner just as well: the callee reaches the
// derived object with a static_cast, no vtable involved.
//===----------------------------------------------------------------------===//

struct PlainBase {
  int tag;
};

struct [[gsl::Owner]] Derived : PlainBase {
  std::string s;
  std::string_view view() const [[clang::lifetimebound]] { return s; }
};

void touch(PlainBase &b [[clang::noescape]],
           std::string_view v [[clang::noescape]]);

void nonpoly_base_upcast() {
  Derived d;
  // expected-warning@+1 {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  touch(d, d.view()); // expected-note {{assumed to be invalidated by this operation}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: the two arguments do not alias, so opening the gate on the
// parameter type alone must not report.
//===----------------------------------------------------------------------===//

struct Impl : IResettable {
  void reset() override {}
};

struct [[gsl::Owner]] Other {
  std::string s;
  std::string_view view() const [[clang::lifetimebound]] { return s; }
};

void disjoint_objects() {
  Impl i;
  Other o;
  resetAndLog(i, o.view()); // no-warning: different objects
}

void const_reference(const IResettable &b [[clang::noescape]],
                     std::string_view v [[clang::noescape]]);

void cannot_mutate() {
  Impl i;
  Other o;
  const_reference(i, o.view()); // no-warning: const cannot reallocate
}

void no_borrow_argument(IResettable &b [[clang::noescape]], int n);

void nothing_to_alias() {
  Impl i;
  no_borrow_argument(i, 3); // no-warning
}
