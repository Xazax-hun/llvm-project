// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-view-on-mutable-global -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"

// A view (gsl::Pointer) created from a mutable global/static owner can be
// invalidated by mutating that owner elsewhere (another function or TU), which
// the intra-procedural analysis cannot see. Flag it. A const owner is safe.

std::string g_str;
const std::string g_const;

std::string_view from_mutable_global() {
  return g_str; // expected-warning {{borrows from a mutable global or static object}}
}

std::string_view from_const_global() {
  return g_const; // no-warning
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

std::vector<int> *pointer_at_global() {
  return &g_vec; // no-warning (points at the object, whose storage is stable)
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
// loan rooted at the global.
[[clang::lifetime_immortal]] std::string_view immortal_element(int i) {
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

// Negative: caching a view of a CONST global into an owner member is safe.
struct [[gsl::Owner]] CachingOwnerConst {
private:
  std::string_view cache; // no-warning

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

// Negatives through the wrapper: a reference/pointer AT the owner member (its
// object storage is stable; only its buffer moves), and a pointer at the whole
// wrapper, are not flagged. Only a borrow into a buffer is.
std::string &ref_at_wrapped_owner() {
  return g_wrap.owner; // no-warning (reference at the stable owner object)
}
Wrapper *ptr_at_wrapper() {
  return &g_wrap; // no-warning (points at the wrapper object)
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

// Negatives: a pointer/reference AT an array element object (its storage is
// stable inline in the array; only its buffer moves) is not flagged.
std::string *ptr_at_array_element() {
  return &g_owner_arr[2]; // no-warning
}
std::string &ref_at_array_element() {
  return g_owner_arr[2]; // no-warning
}

// Negative: a global array of non-owners is not flagged.
int g_int_arr[8];
int *ptr_into_int_array() {
  return &g_int_arr[0]; // no-warning (not an owner; stable global storage)
}
