// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

// `const` on a global excludes it from the borrow-from-mutable-global rule,
// because a const owner cannot be reallocated. That reasoning holds for
// `const std::vector<int>`, but not for an owning smart pointer: `const` applies
// to the pointer, not to what it owns. `const std::unique_ptr<std::vector<int>>`
// still hands out a non-const `std::vector<int>*` from its const `operator->`, so
// `g->push_back(7)` compiles and reallocates the vector a caller may be holding a
// borrow into. One `const` silenced the rule for every owning smart pointer.
//
// The exclusion now asks whether `const` really protects what the object owns.

//===----------------------------------------------------------------------===//
// A const owning smart pointer is still a mutable-owner global.
//===----------------------------------------------------------------------===//

static const std::unique_ptr<std::vector<int>> g_const_up;

// The mutation lives in another function, so the intra-procedural invalidation
// check cannot see it -- the mutable-global rule is the only thing that covers
// this, which is what made the `const` exclusion load-bearing.
// expected-warning@+1 {{borrows from a mutable global or static object}}
static void grow() { g_const_up->push_back(7); }

void borrow_from_const_unique_ptr() {
  grow();
  // Both the dereference of the smart pointer and the later read of the borrow
  // designate storage owned by the global.
  int *p = &(*g_const_up)[0]; // expected-warning {{borrows from a mutable global or static object}}
  grow();
  // The report anchors at the read of the borrow.
  (void)p; // expected-warning {{borrows from a mutable global or static object}}
}

// The non-const spelling was always reported, and must stay reported.
static std::unique_ptr<std::vector<int>> g_up;

void borrow_from_unique_ptr() {
  int *p = &(*g_up)[0]; // expected-warning {{borrows from a mutable global or static object}}
  (void)p; // expected-warning {{borrows from a mutable global or static object}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: here `const` genuinely does protect the owned storage.
//===----------------------------------------------------------------------===//

// A const owner cannot be reallocated at all.
static const std::vector<int> g_const_vec;

void borrow_from_const_vector() {
  const int *p = &g_const_vec[0]; // no-warning
  (void)p;
}

static const std::string g_const_str;

void borrow_from_const_string() {
  const char *p = g_const_str.data(); // no-warning
  (void)p;
}

// A const smart pointer to a CONST owner: the pointee cannot be mutated either,
// so the exclusion still applies.
static const std::unique_ptr<const std::vector<int>> g_const_up_const_pointee;

void borrow_from_const_pointee() {
  const int *p = &(*g_const_up_const_pointee)[0]; // no-warning
  (void)p;
}
