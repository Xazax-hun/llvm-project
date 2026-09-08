// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// An upcast denotes the same object as its operand, so the object's loan has to carry
// through it. The shapes can differ -- the derived and base classes may carry
// different gsl::Pointer annotations, or the base may hold no origins at all -- and
// that mismatch was skipped SILENTLY, where the downcast below it already handled the
// same case by carrying the outer loan and seeding the rest as unknown.
//
// So a member of a base subobject reached through the implicit `this` upcast started
// from an empty origin and carried no loan. A view member bound to an owner member of
// a SIBLING base was therefore not seen as self-referential, while the identical store
// with the owner as a direct member was reported -- and bases are destroyed in reverse
// declaration order, so the owner base is freed before the view base's destructor
// reads it.

volatile char sink;

struct [[gsl::Pointer(char)]] ViewBase {
  string_view v;
  ~ViewBase() { sink = v.data()[0]; }
};

// A plain base holding an owner. It has no origins of its own, which is exactly the
// shape mismatch that used to drop the flow.
struct OwnerBase {
  string owned{"a string long enough to not be SSO"};
};

//===----------------------------------------------------------------------===//
// Reported: the view and the owner are in different subobjects of one object.
//===----------------------------------------------------------------------===//

// The reported shape: owner in a SIBLING BASE.
struct [[gsl::Pointer(char)]] SiblingBases : ViewBase, OwnerBase {
  SiblingBases() { v = owned; } // expected-warning {{member is bound to a sibling member of the same object}}
};

// Reached through a GRANDPARENT base on both sides.
struct [[gsl::Pointer(char)]] ViewMid : ViewBase {};
struct OwnerMid : OwnerBase {};

struct [[gsl::Pointer(char)]] Grandparents : ViewMid, OwnerMid {
  Grandparents() { v = owned; } // expected-warning {{member is bound to a sibling member of the same object}}
};

// The owner as a DIRECT member, which was reported all along -- the two must agree.
struct [[gsl::Pointer(char)]] OwnerAsMember : ViewBase {
  string owned{"a string long enough to not be SSO"};
  OwnerAsMember() { v = owned; } // expected-warning {{member is bound to a sibling member of the same object}}
};

//===----------------------------------------------------------------------===//
// Must stay silent: the borrow is not into the same object.
//===----------------------------------------------------------------------===//

static const char kImmortal[] = "immortal";

struct [[gsl::Pointer(char)]] FromImmortal : ViewBase, OwnerBase {
  FromImmortal() { v = string_view(kImmortal); } // no-warning
};

// Borrowing a member of a DIFFERENT object is not self-referential. (A constructor
// cannot carry lifetime_capture_by(this) -- that is banned separately -- so this is
// spelled as a setter.)
struct [[gsl::Pointer(char)]] FromOther : ViewBase {
  void set(OwnerBase &other [[clang::lifetime_capture_by(this)]]) {
    v = other.owned; // no-warning
  }
};
