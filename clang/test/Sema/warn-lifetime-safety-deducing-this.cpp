// RUN: %clang_cc1 -fsyntax-only -std=c++23 -Wlifetime-safety-soundness -Wno-return-stack-address -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;

// A C++23 explicit object parameter ("deducing this") is an ordinary parameter,
// not the implicit `this`. The lifetimebound parameter->return origin flow must
// still treat it as the object: a [[clang::lifetimebound]] on the explicit
// object parameter `self` means the returned borrow is bound to the object, so a
// borrow into the result that outlives the object must be diagnosed -- just like
// an ordinary implicit-object lifetimebound method.

struct Wrapper {
  string member;

  // Returns the whole object, bound to `self`.
  const Wrapper &id(this const Wrapper &self [[clang::lifetimebound]]) {
    return self;
  }
  // Returns a member view, bound to `self`.
  string_view view(this const Wrapper &self [[clang::lifetimebound]]) {
    return self.member;
  }
};

// The borrow of the object reaches `p` through the deducing-this call, used
// inline as `&w.id().member`; `w` then expires.
void return_self_then_member() {
  const string *p = nullptr;
  {
    Wrapper w;
    p = &w.id().member; // expected-warning {{local variable 'w' does not live long enough}}
  } // expected-note {{destroyed here}}
  (void)p->data(); // expected-note {{later used here}}
}

// A member view returned from a deducing-this method that escapes its object.
string_view escapes_via_member_view() {
  Wrapper w;
  return w.view(); // expected-warning {{stack memory associated with local variable 'w' is returned}} expected-note {{returned here}}
}

//===----------------------------------------------------------------------===//
// A deducing-this method's explicit (non-object) parameters map 1:1 to the
// arguments. The invalidation / annotation helpers must use that mapping too,
// not the implicit-`this` off-by-one -- otherwise a mutating owner argument is
// matched against the object parameter and its invalidation is missed.
//===----------------------------------------------------------------------===//

struct Mutator {
  int n;
  // Non-const deducing-this method that reallocates the owner argument.
  void grow(this Mutator &self [[clang::noescape]],
            string &owner [[clang::noescape]]) {
    self.n++;
    owner.push_back('x'); // reallocates `owner`
  }
};

void mutating_arg_invalidates() {
  Mutator m{0};
  string s = "a long heap-allocated string exceeding the sso buffer size!!";
  string_view v = s; // expected-warning {{may be invalidated by an operation that lifetime safety analysis assumes mutates the owner}}
  m.grow(s);         // expected-note {{assumed to be invalidated by this operation}}
  (void)v.size();
}

//===----------------------------------------------------------------------===//
// A by-reference explicit object parameter is a borrow-holder like any other
// reference parameter: it must be annotated, and escaping its address (or a
// field's) is caught -- just like the implicit `this`.
//===----------------------------------------------------------------------===//

const int *g_field; // expected-note {{escapes to this global storage}}

struct Escaper {
  int field;
  // Unannotated by-reference `self`: requires an annotation.
  void leak_unannotated(this Escaper &self) { // expected-warning {{parameter that can hold a borrow is not annotated for lifetime safety}}
    (void)self.field;
  }
  // Escaping the address of a field of `self` to a global, with `self` annotated
  // [[clang::noescape]], is a noescape violation.
  void leak_field(this const Escaper &self [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    g_field = &self.field;
  }
};

//===----------------------------------------------------------------------===//
// Negatives: the result is used within the object's lifetime.
//===----------------------------------------------------------------------===//

// An annotated explicit object parameter used read-only within the object's
// lifetime is fine.
struct Reader {
  int n;
  void read(this const Reader &self [[clang::noescape]],
            const string &s [[clang::noescape]]) {
    (void)s.data();
    (void)self.n;
  }
};
void read_in_scope() {
  Reader r{0};
  string s = "a long heap-allocated string exceeding the sso buffer size!!";
  r.read(s); // no-warning
}

void use_in_scope() {
  Wrapper w;
  const string *p = &w.id().member;
  (void)p->data(); // no-warning (w still alive)
}
