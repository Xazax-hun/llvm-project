// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-owner-capture -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string_view;

// 'lifetime_capture_by(this)' on a [[gsl::Owner]] type is rejected: an owner is
// meant to own its contents, not to capture a borrow into its (opaque) members,
// which cannot be tracked once the owner is passed elsewhere.

class [[gsl::Owner]] HideOwner {
  string_view hidden;

public:
  void stash(string_view s [[clang::lifetime_capture_by(this)]]) { // expected-warning {{'lifetime_capture_by(this)' names a [[gsl::Owner]] type}}
    hidden = s;
  }
};

// The owner-ness of a class-template specialization is recognized via the
// primary template, even when the explicit specialization does not spell the
// attribute itself. The ban must honor that same fallback -- testing the
// attribute directly on the specialization's record was a soundness bypass
// (the captured borrow lands in the owner's opaque member, untracked, and a
// truthful 'noescape' on the owner-ref params silences every other net).
template <class T> struct [[gsl::Owner]] OwnerTmpl { T v; };
template <> struct OwnerTmpl<int> {
  const int *p = nullptr;

public:
  void set(const int *q [[clang::lifetime_capture_by(this)]]) { p = q; } // expected-warning {{'lifetime_capture_by(this)' names a [[gsl::Owner]] type}}
};

//===----------------------------------------------------------------------===//
// Negatives.
//===----------------------------------------------------------------------===//

// A [[gsl::Pointer]] (view) is the right type to hold a borrow: allowed.
class [[gsl::Pointer]] Viewer {
  string_view v;

public:
  void stash(string_view s [[clang::lifetime_capture_by(this)]]) { v = s; } // no-warning
};

// Capturing into something other than 'this' is not the owner-capture case.
struct Sink {
  string_view v;
};
void capture_into_param(Sink &out,
                        string_view s [[clang::lifetime_capture_by(out)]]); // no-warning

// A plain (un-annotated) type capturing by 'this' is not an owner.
class Plain {
  string_view v;

public:
  void stash(string_view s [[clang::lifetime_capture_by(this)]]) { v = s; } // no-warning
};

// A specialization of a non-owner primary template is likewise not an owner.
template <class T> struct PlainTmpl { T v; };
template <> struct PlainTmpl<int> {
  const int *p = nullptr;

public:
  void set(const int *q [[clang::lifetime_capture_by(this)]]) { p = q; } // no-warning
};

