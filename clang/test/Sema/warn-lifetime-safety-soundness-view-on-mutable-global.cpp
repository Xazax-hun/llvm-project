// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-view-on-mutable-global -Wlifetime-safety-immortal-violation -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"

// A pointer/reference/view that borrows from a mutable global or static owner
// aliases that owner invisibly to the caller: a mutation of the owner elsewhere
// (another function or TU) the intra-procedural analysis cannot see can
// invalidate a view derived from it, and the borrow can dangle. The model bans
// any such borrow once the global is, or transitively contains, a mutable owner.
// A const owner has no such aliasing hazard, but its buffer is still freed by its
// destructor at static destruction, so a borrow of one that outlives the call is
// flagged separately -- see
// warn-lifetime-safety-soundness-global-dtor-order.cpp.
// The one permitted interaction is a method call on the
// global (`g.method()`): the receiver is exempt, but a borrow the method returns
// is checked at its own use/escape.

std::string g_str;
const std::string g_const;

std::string_view from_mutable_global() {
  return g_str; // expected-warning {{borrows from a mutable global or static object}}
}

std::string_view from_const_global() {
  // No aliasing hazard (the global is const), but the returned borrow outlives
  // the call and g_const's buffer is freed at static destruction.
  return g_const; // expected-warning {{borrows from a global or static object with a non-trivial destructor and is returned to the caller}}
}

std::string_view from_static_local() {
  static std::string s;
  return s; // expected-warning {{borrows from a mutable global or static object}}
}

// The same applies to a raw pointer/reference that borrows *into* a mutable
// global owner's contents (not only a GSL view). The borrow may be invalidated
// by a mutation of the global elsewhere.

std::vector<int> g_vec;

int *raw_into_global() {
  return &g_vec[0]; // expected-warning {{borrows from a mutable global or static object}}
}

void raw_into_global_local() {
  int *p = &g_vec[0];
  (void)p; // expected-warning {{borrows from a mutable global or static object}}
}

// A pointer to the whole global owner aliases a mutable owner the caller cannot
// see: through it the caller can mutate the owner (invalidating views) or derive
// a dangling view, so it is flagged. (Annotate with [[clang::lifetime_immortal]]
// if the contract is that the returned object outlives all callers.)
std::vector<int> *pointer_at_global() {
  return &g_vec; // expected-warning {{borrows from a mutable global or static object}}
}

void borrow_into_local() {
  std::vector<int> l;
  int *p = &l[0];
  (void)p; // no-warning (local, not a global)
}

// A view into the CONTENTS of a mutable global owner -- e.g. a std::string
// element of a global std::vector reached via operator[] -- borrows from the
// global just as directly as a view of the whole owner. The borrow surfaces as
// a loan rooted at the global, so it is caught regardless of how it was reached.

std::vector<std::string> g_table;

std::string_view element_of_global_returned(int i) {
  return g_table[i]; // expected-warning {{borrows from a mutable global or static object}}
}

void element_of_global_local(int i) {
  std::string_view v = g_table[i]; // expected-warning {{borrows from a mutable global or static object}}
  (void)v; // expected-warning {{borrows from a mutable global or static object}}
}

// A [[clang::lifetime_immortal]] accessor does not exempt this: the attribute
// promises the function's *result* outlives callers, but a global owner's
// reallocatable buffer is not immortal. The loan-based check still sees the
// loan rooted at the global, and the immortal body verifier likewise rejects
// the non-immortal borrow.
[[clang::lifetime_immortal]] std::string_view immortal_element(int i) { // expected-warning {{returns a borrow of an object the analysis cannot prove is immortal}}
  return g_table[i]; // expected-warning {{borrows from a mutable global or static object}}
}

// Precision: a view into a LOCAL container's element is not a global borrow.
void element_of_local() {
  std::vector<std::string> l;
  std::string_view v = l[0];
  (void)v; // no-warning (local, not a global)
}

// A view cached into the member of a [[gsl::Owner]] still borrows from the
// mutable global, but the owner's contents are opaque to the use/escape pass and
// the dangling read happens in another function (through the owner's member),
// which the intra-procedural analysis cannot follow. The borrow is caught
// loan-based when the view member escapes at function exit still holding a loan
// rooted at the global; the diagnostic anchors at the member declaration (the
// store expression is not available at the escape point).

struct [[gsl::Owner]] CachingOwner {
private:
  std::string_view cache; // expected-warning {{borrows from a mutable global or static object}}

public:
  void refresh() { cache = g_str; }
};

// The same for a view cached from an ELEMENT of a global container. Here the
// element reference `g_table[i]` (a `const std::string &`) is itself a borrow
// into the global, reported at the use, in addition to the member escape.
struct [[gsl::Owner]] CachingOwner2 {
private:
  std::string_view cache; // expected-warning {{borrows from a mutable global or static object}}

public:
  void refresh(int i) { cache = g_table[i]; } // expected-warning {{borrows from a mutable global or static object}}
};

// Caching a view of a CONST global into an owner member is a static-destruction
// hazard: the const global cannot be mutated, but its non-trivial destructor
// frees its buffer at teardown, and the member escape keeps the view alive into
// another object's destruction. The destruction order across TUs is unknowable.
struct [[gsl::Owner]] CachingOwnerConst {
private:
  std::string_view cache; // expected-warning {{borrows from a global or static object with a non-trivial destructor and escapes to global or static storage}}

public:
  void refresh() { cache = g_const; }
};

//===----------------------------------------------------------------------===//
// The global owner reached through a non-owner WRAPPER.
//===----------------------------------------------------------------------===//

// A view can borrow the buffer of an owner that is a member of a non-owner
// global record (`struct W { std::string s; } g_w;`). An accessor returning a
// view (or a raw pointer into the buffer) anchors the loan at the whole `g_w`
// object, whose type is not itself a gsl::Owner -- so the owner-ness is detected
// through the wrapper (the record transitively contains a mutable owner). A
// gsl::Pointer view, or a raw pointer/reference *into* the buffer, is flagged.

struct Wrapper {
  std::string owner;
  std::string_view get() const [[clang::lifetimebound]] { return owner; }
  const char *data() const [[clang::lifetimebound]] { return owner.data(); }
};
Wrapper g_wrap;

std::string_view from_wrapped_global() {
  return g_wrap.get(); // expected-warning {{borrows from a mutable global or static object}}
}

// A raw pointer into the wrapped owner's buffer is flagged too (the pointee is
// the element/char type, not an owner).
const char *raw_into_wrapped_global() {
  return g_wrap.data(); // expected-warning {{borrows from a mutable global or static object}}
}

// Cached into a [[gsl::Owner]] member (escape form), reported at the member.
struct [[gsl::Owner]] WrapCache {
private:
  std::string_view sv; // expected-warning {{borrows from a mutable global or static object}}

public:
  void refresh() { sv = g_wrap.get(); }
};

// A reference/pointer through which a mutable global owner is reachable is
// flagged -- whether the borrowed thing is the owner member itself, or the whole
// wrapper object (the caller can mutate the owner through it, invalidating views
// it derives). A non-owner wrapper that *contains* an owner counts.
std::string &ref_at_wrapped_owner() {
  return g_wrap.owner; // expected-warning {{borrows from a mutable global or static object}}
}
Wrapper *ptr_at_wrapper() {
  return &g_wrap; // expected-warning {{borrows from a mutable global or static object}}
}

// Negative: a wrapper with no owner (only stable scalars) is not flagged.
struct PlainWrapper {
  int value;
  const int *get() const [[clang::lifetimebound]] { return &value; }
};
PlainWrapper g_plain;
const int *from_plain_wrapper() {
  return g_plain.get(); // no-warning (no reallocatable storage)
}

// A reference to a non-owner scalar member of an owner-containing global. The
// `int` itself is stable (it lives inline in `g_wc`, only the owner's buffer
// moves), so this reference does not dangle -- but at the loan level it is
// indistinguishable from a raw pointer into the owner's buffer returned by an
// accessor (`g_wc.owner.data()`), so the broad rule flags it too. Mark such an
// accessor [[clang::lifetime_immortal]] if the reference is known to be safe.
struct WithCounter {
  std::string owner;
  int counter;
};
WithCounter g_wc;
int &ref_at_nonowner_member() {
  return g_wc.counter; // expected-warning {{borrows from a mutable global or static object}}
}

//===----------------------------------------------------------------------===//
// Global ARRAY of owners.
//===----------------------------------------------------------------------===//

// A global array of owners owns reallocatable storage per element; a view of an
// element borrows that buffer. The loan roots at the array variable, whose own
// type is the array (not an owner), so the element type must be tested.
std::string g_owner_arr[4];

std::string_view view_of_array_element() {
  return g_owner_arr[2]; // expected-warning {{borrows from a mutable global or static object}}
}

// A pointer/reference to an array element that IS a mutable owner reaches that
// owner (the caller can mutate it / derive a dangling view), so it is flagged.
std::string *ptr_at_array_element() {
  return &g_owner_arr[2]; // expected-warning {{borrows from a mutable global or static object}}
}
std::string &ref_at_array_element() {
  return g_owner_arr[2]; // expected-warning {{borrows from a mutable global or static object}}
}

// Negative: a global array of non-owners is not flagged.
int g_int_arr[8];
int *ptr_into_int_array() {
  return &g_int_arr[0]; // no-warning (not an owner; stable global storage)
}

//===----------------------------------------------------------------------===//
// The permitted interaction: a method call on the global.
//===----------------------------------------------------------------------===//

struct Owning {
  std::string s;
  void mutate() { s.push_back('x'); }       // non-const
  std::string_view view() const [[clang::lifetimebound]] { return s; }
};
Owning g_own;

// A method call on the global is the one allowed way to interact with it: the
// receiver is a transient access, not a borrow the caller keeps. The receiver is
// not flagged -- neither for a mutating method nor for a borrow-returning one.
void method_call_on_global() {
  g_own.mutate();    // no-warning (receiver is not a borrow)
  g_own.s.clear();   // no-warning (owner member method call)
}

// But a borrow the method *returns* is still flagged at its own use/escape.
std::string_view returned_borrow_flagged() {
  return g_own.view(); // expected-warning {{borrows from a mutable global or static object}}
}
void sv_sink(std::string_view sv [[clang::noescape]]);
void returned_borrow_used_locally() {
  std::string_view v = g_own.view();
  sv_sink(v); // expected-warning {{borrows from a mutable global or static object}}
}

// A receiver that *selects* or *computes* a borrow is not the simple
// `global.method()` form -- it extracts a borrow first, so it is flagged.
Owning g_own2;
void selecting_receiver(bool c) {
  (c ? g_own : g_own2).mutate(); // expected-warning {{borrows from a mutable global or static object}}
}

// Negative: reading a by-value owner is a copy, not a borrow.
std::string copy_of_global() {
  return g_str; // no-warning (by-value copy, no borrow escapes)
}

// The method-call exemption is ONLY for the global owner itself as receiver. A
// separate borrow OF the global -- a local view bound to it -- used as a member
// call receiver is still flagged: the receiver is the held borrow `v`, not the
// global. (Otherwise `v.front()` after a cross-function mutation of `g_str`
// would be a silent use-after-free.)
void view_receiver_is_flagged() {
  std::string_view v = g_str; // (silent at construction; the borrow is reported at the use)
  (void)v.size();             // expected-warning {{borrows from a mutable global or static object}}
}
