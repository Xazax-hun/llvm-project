// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

using std::vector;

// A compound assignment reads AND writes its left operand, and both happen AFTER
// the right operand is evaluated. So when the RHS invalidates what the LHS
// borrows, the write goes through a dangling borrow:
//
//   int *p = &v[0];
//   p[0] += v.emplace_back(5);   // emplace_back reallocates, then p[0] is written
//
// The LHS's own use is registered when the LHS is evaluated, which is before the
// RHS, so nothing kept the borrow live across the invalidation and the write was
// not modelled at all. The plain-assignment spelling reported this, but only
// because C++17 sequences its RHS before its LHS -- putting the LHS use after the
// invalidation by luck of the ordering rather than by anything modelling the
// write.

//===----------------------------------------------------------------------===//
// Every compound operator, and both LHS spellings.
//===----------------------------------------------------------------------===//

void compound_add() {
  vector<int> v;
  v.push_back(1);
  int *p = &v[0]; // expected-warning {{object whose reference is captured is later invalidated}}
  p[0] += v.emplace_back(5); // expected-note {{invalidated here}}
  // expected-note@-1 {{later used here}}
}

void compound_shift() {
  vector<int> v;
  v.push_back(1);
  int *p = &v[0]; // expected-warning {{object whose reference is captured is later invalidated}}
  p[0] <<= v.emplace_back(5); // expected-note {{invalidated here}}
  // expected-note@-1 {{later used here}}
}

// Written through a dereference rather than a subscript.
void compound_through_deref() {
  vector<int> v;
  v.push_back(1);
  int *p = &v[0]; // expected-warning {{object whose reference is captured is later invalidated}}
  *p += v.emplace_back(5); // expected-note {{invalidated here}}
  // expected-note@-1 {{later used here}}
}

// The plain-assignment spelling, which was reported before this change.
void plain_assign() {
  vector<int> v;
  v.push_back(1);
  int *p = &v[0]; // expected-warning {{object whose reference is captured is later invalidated}}
  p[0] = v.emplace_back(5); // expected-note {{invalidated here}}
  // expected-note@-1 {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

volatile int isink;

// No invalidation anywhere.
void no_invalidation() {
  vector<int> v;
  v.push_back(1);
  int *p = &v[0];
  p[0] += 5; // no-warning
  isink = v[0];
}

// The invalidation happens AFTER the write, so the borrow was still valid when
// it was used.
void invalidation_after_write() {
  vector<int> v;
  v.push_back(1);
  int *p = &v[0];
  p[0] += 1; // no-warning
  v.emplace_back(9);
  isink = v[0];
}

// Compound assignment on plain locals borrows nothing.
void scalars() {
  int a = 1, b = 2;
  a += b;
  a <<= 1;
  a |= 4; // no-warning
  isink = a;
}

// A pointer compound additive assignment keeps the pointer in the same
// allocation, and its result still carries the pointer's loans.
void pointer_arithmetic() {
  int arr[8] = {};
  int *p = arr;
  p += 3; // no-warning
  isink = *p;
}
