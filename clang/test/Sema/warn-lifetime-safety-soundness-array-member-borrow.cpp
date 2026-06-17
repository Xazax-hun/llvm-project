// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

#include "Inputs/lifetime-analysis.h"
using std::vector;
using std::string_view;

// A C-array member that holds a borrow must be recognized like the scalar
// member: a ConstantArrayType is not pointer-like, is neither an owner- nor a
// pointer-of-indirection, and its getAsCXXRecordDecl() is null, so an
// array-of-pointers / array-of-views / array-of-owner-of-indirection member
// previously hid the borrow the record holds. The recognition checks now peel
// array dimensions on the field type.

//===----------------------------------------------------------------------===//
// (1) unknown-ownership: a record whose only indirection-holding member is a
// C-array of raw pointers/views is a borrow-holding non-gsl type.
//===----------------------------------------------------------------------===//

struct PtrArr {
  int *arr[2];
};

PtrArr make_ptr_arr(int x) {
  return PtrArr{{&x, nullptr}}; // expected-warning {{type 'PtrArr' can hold a borrow but is annotated neither [[gsl::Owner]] nor [[gsl::Pointer]]}}
}

int g;
void use_ptr_arr_inline() {
  g = *make_ptr_arr(7).arr[0]; // expected-warning {{type 'PtrArr' can hold a borrow but is annotated neither [[gsl::Owner]] nor [[gsl::Pointer]]}}
}

// Multidimensional, and an array of references-wrappers-as-pointers, behave the
// same (peeling removes every dimension).
struct PtrArr2D {
  int *grid[2][3];
};
PtrArr2D mk2d(int x) {
  return PtrArr2D{{{&x}}}; // expected-warning {{type 'PtrArr2D' can hold a borrow but is annotated neither}}
}

//===----------------------------------------------------------------------===//
// (2) owner-of-indirection: a C-array member of a container whose elements hold
// borrows is rejected at the record definition, like the scalar member.
//===----------------------------------------------------------------------===//

struct ViewVecArr {
  vector<string_view> arr[2]; // expected-warning {{is a container whose element type holds a borrow}}
};

// Scalar control (already rejected before this fix) -- kept to show parity.
struct ViewVecScalar {
  vector<string_view> v; // expected-warning {{is a container whose element type holds a borrow}}
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// Non-borrow array members: no warning.
struct PlainArrays {
  int a[4];
  double d[2][3];
  char c[8];
};
PlainArrays mk_plain() { return PlainArrays{}; } // no-warning

// A C-array of a plain value-only struct is fine.
struct Val {
  int x;
};
struct ValArr {
  Val v[3];
};
ValArr mk_val() { return ValArr{}; } // no-warning
