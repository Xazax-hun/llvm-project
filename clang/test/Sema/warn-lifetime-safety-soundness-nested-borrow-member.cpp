// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

// A borrow held one record deeper than the annotated type. Both checks that
// should have covered it stopped at the member's own type:
//
//  - The [[gsl::Owner]] public-borrow-member check tested whether the FIELD's
//    type is a pointer/reference/view, so wrapping the offending member in one
//    plain struct silenced it.
//  - VisitDeclStmt asks the ownership question of the DECLARED type, so an
//    annotated outer type reports nothing; the plain sub-aggregate in the
//    initializer skipped itself assuming the declaration had covered it.
//
// Annotating the wrapper therefore silenced what the direct member form
// reports, even though the borrow is just as reachable and the annotated type
// is still opaque to the analysis.

struct Raw {
  // Reported once for `OwnerBase` below, which inherits this member into an
  // owner. The check walks a non-owner base's fields as if they were the
  // owner's own.
  std::vector<int> *v; // expected-warning {{public data member 'v' of a [[gsl::Owner]] type can hold a borrow}}
  ~Raw();
};

//===----------------------------------------------------------------------===//
// [[gsl::Owner]]: a public member that holds a borrow, directly or nested.
//===----------------------------------------------------------------------===//

struct [[gsl::Owner]] OwnerDirect {
  std::vector<int> *v; // expected-warning {{public data member 'v' of a [[gsl::Owner]] type can hold a borrow}}
};

struct [[gsl::Owner]] OwnerNested {
  Raw r; // expected-warning {{public data member 'r' of a [[gsl::Owner]] type can hold a borrow}}
};

// Two records deep is the same question.
struct Mid {
  Raw r;
};
struct [[gsl::Owner]] OwnerDeep {
  Mid m; // expected-warning {{public data member 'm' of a [[gsl::Owner]] type can hold a borrow}}
};

// Reached through a base rather than a member; reported at Raw::v above.
struct [[gsl::Owner]] OwnerBase : Raw {};

//===----------------------------------------------------------------------===//
// A borrow-holding sub-aggregate in the initializer of an ANNOTATED
// declaration. The escaping-temporary spelling of the same thing was always
// reported; the declaration spelling was not.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] ViewNested {
  // A view that owns nothing must not have a member that frees; reported
  // independently by the view-subobject rule.
  Raw r; // expected-warning {{member 'Raw' of [[gsl::Pointer]] 'ViewNested' may deallocate in its destructor}}
};

void declaration_form() {
  std::vector<int> v;
  ViewNested g{{&v}}; // expected-warning {{type 'Raw' can hold a borrow but is annotated neither [[gsl::Owner]] nor [[gsl::Pointer]]}}
}

// expected-warning@+1 {{parameter that can hold a borrow is not annotated for lifetime safety}}
ViewNested temporary_form(std::vector<int> *p) {
  return ViewNested{{p}}; // expected-warning {{type 'Raw' can hold a borrow but is annotated neither [[gsl::Owner]] nor [[gsl::Pointer]]}}
}

//===----------------------------------------------------------------------===//
// Must stay silent: nothing here puts a borrow inside the annotated type, and
// an encapsulated member is the documented way to hold one.
//===----------------------------------------------------------------------===//

struct PlainData {
  int a;
  double b;
};

struct [[gsl::Owner]] OwnerPlainMember {
  PlainData d; // no-warning: holds no borrow
};

struct [[gsl::Pointer]] ViewPlainMember {
  const char *p; // no-warning: a view is meant to hold a borrow
  PlainData d;
};

// expected-warning@+2 {{parameter that can hold a borrow is not annotated for lifetime safety}}
struct [[gsl::Owner]] OwnerPrivate {
  OwnerPrivate(std::vector<int> *p) : r{p} {} // expected-warning {{type 'Raw' can hold a borrow but is annotated neither [[gsl::Owner]] nor [[gsl::Pointer]]}}

private:
  Raw r; // no-warning: encapsulated
};

// A plain (unannotated) declaration is still reported exactly once, at the
// declaration -- the sub-aggregate skip must stay in place for it.
struct Raw2 {
  int *p;
};

void plain_aggregate_reported_once() {
  int x = 1;
  Raw2 h{&x}; // expected-warning {{type 'Raw2' can hold a borrow but is annotated neither [[gsl::Owner]] nor [[gsl::Pointer]]}}
}
