// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A base subobject is a borrow-holding subobject of `this` exactly as a member is, so
// a base initialized from a MEMBER of the derived class binds the object to itself.
// Only the member spelling produced a FieldStore, so only it was recognised as
// self-referential; the base spelling modelled the deposit but recorded no store, and
// the checker never looked.
//
// The base form is the worse of the two, and for a different reason: a base is
// initialized BEFORE the members and destroyed AFTER them, so the borrow refers to a
// member that is not constructed yet and is read again once it is gone -- with
// nothing moved or mutated. That is the flush-on-destroy mixin. Reusing the
// self-referential wording ("mutating or moving the object can invalidate this")
// would describe a hazard this code never triggers, and read as a false positive.

volatile char sink;

struct [[gsl::Pointer]] Flusher {
  const string *target;
  Flusher(const string &t [[clang::lifetimebound]]) : target(&t) {}
  ~Flusher() { sink = target->data()[0]; }
};

//===----------------------------------------------------------------------===//
// Reported: the borrow names a subobject of the object being constructed.
//===----------------------------------------------------------------------===//

// The reported shape.
struct [[gsl::Pointer]] Session : Flusher {
  string buffer;
  // expected-warning@+1 {{base class 'Flusher' is initialized with a member of the derived object}}
  Session() : Flusher(buffer), buffer("x") {}
};

// Declaring the member first does not help: bases are still initialized first and
// destroyed last, whatever the member order.
struct [[gsl::Pointer]] SessionReordered : Flusher {
  string buffer;
  // The assignment in the body also invalidates the borrow the base already holds,
  // which is a second, correct report about the same design.
  // expected-warning@+3 {{base class 'Flusher' is initialized with a member of the derived object}}
  // expected-warning@+2 {{object whose reference is captured is later invalidated}}
  // expected-note@+1 {{invalidated here}} expected-note@+1 {{later used here}}
  SessionReordered() : Flusher(buffer) { buffer = "x"; }
};

// The MEMBER spelling of the same relationship, reported all along -- with the other
// wording, since there the hazard really is mutation or movement.
struct [[gsl::Pointer]] SessionMember {
  Flusher f;
  string buffer;
  // expected-warning@+1 {{'Flusher' member is bound to a sibling member of the same object}}
  SessionMember() : f(buffer), buffer("x") {}
};

//===----------------------------------------------------------------------===//
// Must stay silent: the borrowed object is not part of the constructed one.
//===----------------------------------------------------------------------===//

static const string kGlobal = "global";

// A global outlives every object built from it.
struct [[gsl::Pointer]] FromGlobal : Flusher {
  FromGlobal() : Flusher(kGlobal) {} // no-warning
};

// A parameter belongs to the caller. (This is the canonical lifetimebound
// constructor, so it must stay clean.)
struct [[gsl::Pointer]] FromParam : Flusher {
  explicit FromParam(const string &s [[clang::lifetimebound]]) : Flusher(s) {} // no-warning
};

// A member of a DIFFERENT object.
struct Other {
  string buffer;
};

struct [[gsl::Pointer]] FromOther : Flusher {
  explicit FromOther(Other &o [[clang::lifetimebound]]) : Flusher(o.buffer) {} // no-warning
};
