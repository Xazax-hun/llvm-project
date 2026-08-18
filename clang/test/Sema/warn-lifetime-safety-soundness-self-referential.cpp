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

// A '[[clang::lifetimebound]]' accessor re-roots its result at the object and
// projects `.*` -- "a member of this object, identity unknown". That is still a member
// borrow, so laundering the store through such an accessor is reported exactly as the
// direct store is. It previously was not, which made `view = raw();` accepted where
// `view = str;` was refused, and left the store unrecorded so a later mutation in
// another method had no live borrow to invalidate. The two annotations that produce
// the shape are the two the analysis itself asks for, so this is the form an adopter
// arrives at by following its advice.
struct ViaAccessor {
  string str;
  string_view view;
  string_view raw() const [[clang::lifetimebound]] { return str; }
  void f() {
    view = raw(); // expected-warning {{member is bound to a sibling member of the same object}}
  }
};

// A borrow of the object's own IDENTITY is reported too, and should be: the
// diagnostic is about mutating OR MOVING the object, and moving is exactly what
// invalidates a self-pointer -- `Identity b = a;` leaves `b.self` pointing at `a`.
// (In unannotated code the shape does not arise: a raw self-pointer member is already
// refused by -Wlifetime-safety-multilevel-indirection.)
struct Identity {
  Identity *self = nullptr;
  Identity *me() [[clang::lifetimebound]];
  void f() {
    self = me(); // expected-warning {{member is bound to a sibling member of the same object}}
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


// A self-referential binding established in the constructor MEMBER-INITIALIZER
// list (not the body) is detected too: the member-init is a store into the
// member just like `view = buf;` in the body.
struct InitListSelfRef {
  string buf;
  string_view view;
  InitListSelfRef()
      : buf("long heap-backed content exceeding the small-string buffer"),
        view(buf) {} // expected-warning {{self-referential}}
};

// Negative: a member-initializer binding the view to a constructor PARAMETER
// (an external object, not a sibling member) is not self-referential.
struct InitListExternal {
  string_view view;
  InitListExternal(string_view ext [[clang::lifetimebound]]) : view(ext) {} // no-warning
};
