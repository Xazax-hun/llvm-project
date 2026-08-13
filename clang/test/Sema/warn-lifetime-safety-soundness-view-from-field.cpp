// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;

// A view into an OWNER field is a borrow of that field's heap buffer. Mutating
// the field -- directly, or via a method on the containing object -- can
// reallocate the buffer and invalidate the view. The safe model tracks this by
// issuing a field-rooted loan for the borrow (so a known mutator on the field
// invalidates it precisely) and by treating any non-const member call on the
// object as invalidating views into its owner fields.

struct S {
  string field;
  void grow() { field += "x"; } // non-const: assumed to mutate `field`

  // A borrow-returning accessor (lifetimebound on `this`) must stay clean: it
  // does not mutate, and the returned view is still bound to `this`.
  string_view view() const [[clang::lifetimebound]] { return field; }

  // Variant A: the field is mutated *directly* in the same function. The known
  // mutator (`operator+=`) invalidates the view precisely.
  int directMutation() {
    string_view sv = field; // expected-warning {{object whose reference is captured is later invalidated}}
    field += "x";           // expected-note {{invalidated here}}
    return sv.size();       // expected-note {{later used here}}
  }

  // Variant B: the field is mutated via a non-const helper method on `this`.
  // The mutation is hidden in the callee, so it is an *assumed* invalidation.
  int mutationViaSelfMethod() {
    string_view sv = field; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    grow();                 // expected-note {{assumed to be invalidated by this operation}}
    return sv.size();
  }

  // Negative: a const method between the borrow and the use does not mutate.
  int constMethodIsClean() {
    string_view sv = field;
    (void)view(); // no-warning (const accessor)
    return sv.size();
  }
};

// Variant C: the field is mutated via a non-const method on a *local* object.
int mutationViaLocalObjectMethod() {
  S s;
  string_view sv = s.view(); // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  s.grow();                  // expected-note {{assumed to be invalidated by this operation}}
  return sv.size();
}

//===----------------------------------------------------------------------===//
// Precision: a field mutation invalidates only *that* field, not its siblings
// or the enclosing object.
//===----------------------------------------------------------------------===//

struct TwoFields {
  vector<int> a;
  vector<int> b;
};

// Mutating `a` must not invalidate an iterator into `b`.
int siblingFieldIsClean() {
  TwoFields s;
  auto it = s.b.begin();
  s.a.push_back(1); // no-warning (different field)
  return *it;
}

// Mutating a field must not invalidate a pointer to the whole object.
int pointerToObjectIsClean() {
  TwoFields s;
  TwoFields *p = &s;
  s.a.push_back(1); // no-warning (object storage is unaffected)
  return p->a[0];
}

//===----------------------------------------------------------------------===//
// A non-const member call is assumed to mutate, regardless of whether it also
// *returns* a borrow (lifetimebound / GSL accessor): returning a borrow says
// nothing about whether the call reallocates a field.
//===----------------------------------------------------------------------===//

struct LB {
  string buf;
  // Non-const, lifetimebound on `this`, and it mutates -- must NOT be excluded.
  string_view mutate() [[clang::lifetimebound]] { return buf; }
  int f() {
    string_view sv = buf; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
    (void)mutate();       // expected-note {{assumed to be invalidated by this operation}}
    return sv.size();
  }
};

//===----------------------------------------------------------------------===//
// The owner can be reached transitively (a field of a field) or through a base
// class; a non-const method on the object still invalidates a view into it.
//===----------------------------------------------------------------------===//

struct Inner { string s; };
struct Outer {
  Inner inner;
  void touch(); // non-const; may reallocate inner.s
};
int transitiveOwnerField() {
  Outer o;
  string_view sv = o.inner.s; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  o.touch();                  // expected-note {{assumed to be invalidated by this operation}}
  return sv.size();
}

struct Base { string baseBuf; };
struct Derived : Base {
  void touch(); // non-const; may reallocate the inherited baseBuf
};
int inheritedOwnerField() {
  Derived d;
  string_view sv = d.baseBuf; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  d.touch();                  // expected-note {{assumed to be invalidated by this operation}}
  return sv.size();
}

//===----------------------------------------------------------------------===//
// A `const` owner field can never be reallocated, so a view into it is clean.
//===----------------------------------------------------------------------===//

struct ConstHolder {
  const string cbuf;
  void touch(); // cannot mutate cbuf
};
int constOwnerFieldIsClean(ConstHolder &c [[clang::noescape]]) {
  string_view sv = c.cbuf;
  c.touch(); // no-warning (const field is immutable)
  return sv.size();
}

//===----------------------------------------------------------------------===//
// Cross-instance precision: two distinct local variables are distinct objects,
// so mutating one cannot reach a view into the other. This used to be reported:
// a field borrow was rooted at the FieldDecl and thus instance-insensitive, so
// `s1.field` and `s2.field` were indistinguishable. Field-sensitive access paths
// root the borrow at the variable, which makes the two provably disjoint.
//===----------------------------------------------------------------------===//

int crossInstanceIsPrecise() {
  S s1, s2;
  string_view sv = s1.field;
  s2.grow(); // no-warning: mutating 's2' cannot reach a view into 's1'
  return sv.size();
}

// Aliasing is still caught: a reference is another NAME for an object, not
// another object, so its borrows are rooted at the referent.
int aliasedInstanceStillReports() {
  S s1;
  S &r = s1;
  string_view sv = s1.field; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  r.grow();                  // expected-note {{assumed to be invalidated by this operation}}
  return sv.size();
}

//===----------------------------------------------------------------------===//
// An array of owners is an owner field too: a view into one element is
// invalidated by a non-const method on the object (which may reallocate it).
//===----------------------------------------------------------------------===//

struct ArrHolder {
  string bufs[4];
  void touch(); // non-const; may reallocate an element
};
int arrayOfOwnersField() {
  ArrHolder a;
  string_view sv = a.bufs[0]; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  a.touch();                  // expected-note {{assumed to be invalidated by this operation}}
  return sv.size();
}



// A borrow of an OWNER field requires the field to have stable storage. When the
// base object is a temporary (`Holder{}.s`), the field access is an xvalue: the
// owner subobject lives only as long as the temporary, and a lifetime-extended-
// temporary reference does not give it borrowable storage. The borrow must NOT be
// laundered into a tracked field-rooted loan (which would never expire); it stays
// lost, surfacing as -Wlifetime-safety-lost-loan -- matching `const string& r =
// string(...)` bound directly to a temporary.
struct TempHolder {
  string s;
};
string_view viewOfTempSubobjectField() {
  const string &r = TempHolder{}.s;
  return r; // expected-warning {{lifetime safety cannot track}}
}

struct OuterTemp {
  TempHolder in;
};
string_view viewOfNestedTempSubobjectField() {
  const string &r = OuterTemp{}.in.s;
  return r; // expected-warning {{lifetime safety cannot track}}
}

// Negative: a field of a STABLE local object still gets a tracked field loan, so
// a later mutation of that field is caught (not turned into lost-loan).
int viewOfLocalField() {
  TempHolder h;
  string_view sv = h.s; // expected-warning {{object whose reference is captured is later invalidated}}
  h.s.clear();          // expected-note {{invalidated here}}
  return sv.size();     // expected-note {{later used here}}
}
