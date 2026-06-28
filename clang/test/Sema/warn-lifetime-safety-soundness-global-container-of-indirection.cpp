// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-owner-of-indirection \
// RUN:   -Wlifetime-safety-pointer-of-indirection -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string;
using std::string_view;
using std::vector;
using std::span;

// A global "container of indirection" -- an owner/pointer whose elements or
// pointees are themselves borrows -- is banned by the model. A local of such a
// type is flagged at its declaration; a global's declaration may live outside
// the analyzed region, so a *use* of it inside analyzed code is flagged here.

vector<string_view> g_views;        // owner of indirection
span<int *> g_span;                 // pointer of indirection
vector<int> g_ints;                 // plain owner: safe, must stay silent
string g_str;                       // plain owner: safe, must stay silent

// A lifetime_immortal accessor must not launder a borrow out of such a global:
// the element lives in the global's storage, but the element's pointee does
// not, so the promise is a lie. The use of g_views is flagged.
[[clang::lifetime_immortal]] string_view firstEntry() {
  return g_views[0]; // expected-warning {{is a container whose element type holds a borrow}}
}

void use_owner_of_indirection() {
  g_views.push_back(string_view{}); // expected-warning {{is a container whose element type holds a borrow}}
}

void use_pointer_of_indirection() {
  auto b = g_span.begin(); // expected-warning {{is a view whose pointee type holds a borrow}}
  (void)b;
}

// Controls: plain owner globals are trackable and must not be flagged.
void read_plain_owner() {
  g_ints.push_back(1); // no-warning
  g_str.clear();       // no-warning
}
