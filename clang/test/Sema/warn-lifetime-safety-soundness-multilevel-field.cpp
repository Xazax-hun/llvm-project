// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-multilevel-indirection -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string_view;

// The single-level-of-indirection rule applies to data members of a tracked
// [[gsl::Owner]]/[[gsl::Pointer]] record too: such a record is a leaf whose
// fields are not modeled individually, so a member that is itself a multi-level
// indirection (a pointer/reference to a pointer/reference/view) drops the borrow
// flowing through it. Reject it at the record's definition.

struct [[gsl::Pointer]] PtrRefMember {
  const char *&r; // expected-warning {{field 'r' uses more than one level of indirection}}
};

struct [[gsl::Pointer]] PtrPtrMember {
  int **pp; // expected-warning {{field 'pp' uses more than one level of indirection}}
};

struct [[gsl::Owner]] OwnerPtrPtr {
  int **pp; // expected-warning {{field 'pp' uses more than one level of indirection}}
};

struct [[gsl::Pointer]] View {
  const char *p;
};
struct [[gsl::Pointer]] PtrToView {
  View *v; // expected-warning {{field 'v' uses more than one level of indirection}}
};

// Controls: a single level of indirection in a tracked record is fine.
struct [[gsl::Pointer]] OkPtr {
  const char *p; // no-warning
};
struct [[gsl::Pointer]] OkRef {
  const int &r; // no-warning
};
struct [[gsl::Pointer]] OkView {
  string_view sv; // no-warning: a view is a single level
};

// Control: a multi-level member in a PLAIN (non-gsl) record is not flagged here
// -- such a record is rejected as unknown-ownership at its use instead.
struct PlainPtrPtr {
  int **pp; // no-warning
};
