// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-self-referential -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A self-referential borrow binds a view/pointer member to a sibling member of
// the SAME object. The object becomes self-referential: mutating or moving it
// invalidates the view, and the view->member relationship is invisible once the
// object is passed to another function (and is opaque for an annotated
// owner/pointer type). The store itself is flagged at its source.

struct S {
  string_view view;
  string str;
  void cache() {
    view = str; // expected-warning {{member is bound to a sibling member of the same object, making the object self-referential}}
  }
};

// `this->view = this->str` via a conversion operator (string -> string_view).
struct ThisCase {
  string str;
  string_view view;
  void setup() {
    this->view = this->str; // expected-warning {{self-referential}}
  }
};

// Through a local object (`s.view = s.str`).
void localObject() {
  S s;
  s.view = s.str; // expected-warning {{self-referential}}
}

// An annotated [[gsl::Owner]] type is opaque, so this is the only signal.
struct [[gsl::Owner]] OwnerCache {
  string buffer;
  string_view token;
  void cache() {
    token = buffer; // expected-warning {{self-referential}}
  }
};

// The borrow may be laundered through a function: the detection is loan-based,
// not syntactic, so it still fires when the sibling member is borrowed via a
// lifetimebound call rather than assigned directly.
string_view pick(const string &s [[clang::lifetimebound]]);
struct Laundered {
  string_view view;
  string str;
  void setup() {
    view = pick(str); // expected-warning {{self-referential}}
  }
};

// The owner may be reached through a sub-object (member-of-member).
struct Inner {
  string str;
};
struct Nested {
  string_view view;
  Inner inner;
  void setup() {
    view = inner.str; // expected-warning {{self-referential}}
  }
};

//===----------------------------------------------------------------------===//
// Negatives: must NOT fire.
//===----------------------------------------------------------------------===//

// A lifetimebound-`this` accessor whose result borrows the object's IDENTITY
// but not a member. The result carries the object loan but no member loan, so
// it is not a self-referential member borrow. (Requires the "borrows a member"
// condition, not just "shares the object identity".)
struct Identity {
  string_view view;
  string_view borrowThis() [[clang::lifetimebound]];
  void f() {
    view = borrowThis(); // no-warning (borrows the object, not a member)
  }
};


struct Other {
  string str;
};

// Binding to a sibling that is NOT the same object (a different instance).
void differentObject(Other &o) {
  S s;
  s.view = o.str; // no-warning (different object)
  (void)s;
}

// Binding to an external (non-member) owner.
void externalOwner(string &ext) {
  S s;
  s.view = ext; // no-warning (not a sibling member)
  (void)s;
}

// View-to-view assignment between siblings (no owner borrowed).
struct TwoViews {
  string_view a;
  string_view b;
  void copy() {
    a = b; // no-warning (b is not an owner member)
  }
};

// Assigning a member view from a parameter view.
struct Holder {
  string_view view;
  void set(string_view incoming) {
    view = incoming; // no-warning
  }
};

//===----------------------------------------------------------------------===//
// Across the base/derived boundary: a view member in a BASE subobject bound to
// a member of the DERIVED class. The complete object is still self-referential,
// and destruction order (derived members are destroyed before the base
// destructor runs) makes it especially dangerous. The detection keys on the
// most-derived receiver type, not the class that declares the view member.
//===----------------------------------------------------------------------===//

struct Base {
  string_view cached;
  // Capture into `this` of an argument that is a derived member (below).
  void observe(string_view v [[clang::lifetime_capture_by(this)]]) { cached = v; }
};

struct DerivedDirect : Base {
  string data;
  void cache() {
    cached = data; // expected-warning {{self-referential}}
  }
};

struct DerivedCapture : Base {
  string data;
  void cache() {
    observe(data); // expected-warning {{self-referential}}
  }
};

// Negative: a base-subobject view bound to an EXTERNAL argument (not a member of
// the complete object) is not self-referential.
struct DerivedExternal : Base {
  void cache(string_view ext) {
    observe(ext); // no-warning
  }
};

