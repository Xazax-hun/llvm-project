// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

using std::vector;

// Subscripting reads the BASE in order to follow it, exactly as a dereference
// does. Nothing modelled that read, so a subscript whose INDEX invalidates what
// the base borrows went unreported: the base is used anyway, after the index has
// already freed it.
//
//   int *p = new int(7);
//   sink = p[(delete p, 0)];   // the index frees p, then p is read
//
// The dereference spelling `*(delete p, p)` was caught all along, because the
// deref path registers that read.

volatile int sink;

void index_frees_the_base() {
  int *p = new int(7); // expected-warning {{allocated object does not live long enough}}
  sink = p[(delete p, 0)]; // expected-note {{freed here}}
  // expected-note@-1 {{later used here}}
}

// The dereference spelling of the same hazard.
void index_frees_the_base_deref() {
  int *p = new int(7); // expected-warning {{allocated object does not live long enough}}
  sink = *(delete p, p); // expected-note {{freed here}}
  // expected-note@-1 {{later used here}}
}

// A WRITE through the subscript still reads the base to find the element.
void write_through_subscript() {
  int *p = new int(7); // expected-warning {{allocated object does not live long enough}}
  p[(delete p, 0)] = 3; // expected-note {{freed here}}
  // expected-note@-1 {{later used here}}
}

// The invalidation flavour: the index reallocates the container the base borrows.
void index_invalidates_the_base() {
  vector<int> v;
  v.push_back(1);
  int *p = &v[0]; // expected-warning {{object whose reference is captured is later invalidated}}
  sink = p[(v.emplace_back(9), 0)]; // expected-note {{invalidated here}}
  // expected-note@-1 {{later used here}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: ordinary subscripting, which is everywhere.
//===----------------------------------------------------------------------===//

void plain_array() {
  int arr[4] = {1, 2, 3, 4};
  sink = arr[2]; // no-warning
}

void write_plain_array() {
  int arr[4] = {};
  arr[1] = 5; // no-warning
  sink = arr[1];
}

void container() {
  vector<int> v;
  v.push_back(1);
  sink = v[0]; // no-warning
}

void borrow_stays_valid() {
  vector<int> v;
  v.push_back(1);
  int *p = &v[0];
  sink = p[0]; // no-warning
}

void after_pointer_arithmetic() {
  int arr[8] = {};
  int *p = arr + 2;
  sink = p[1]; // no-warning
}

void nested_subscript() {
  int a[2][2] = {{1, 2}, {3, 4}};
  sink = a[1][1]; // no-warning
}
