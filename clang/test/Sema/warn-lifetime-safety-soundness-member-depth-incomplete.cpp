// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-soundness -Wno-unused -verify %s

#include "Inputs/lifetime-analysis.h"

// The single-indirection rule on data members is measured from the member's
// referent type, which has to be COMPLETE for the measurement to mean anything.
// The check used to run when the enclosing record was completed, so a
// forward-declared referent contributed no fields yet and a depth-2 member looked
// like depth 1:
//
//   struct Fwd;
//   struct [[gsl::Pointer]] H { Fwd &f; };      // measured as depth 1
//   struct Fwd { std::string_view sv; };        // completed afterwards
//
// Defining Fwd first reported the member, so declaration order alone decided
// whether the rule applied. The check now runs at TU end, where every type in
// the TU is complete.

//===----------------------------------------------------------------------===//
// Referent completed AFTER the holder: the order that used to slip through.
//===----------------------------------------------------------------------===//

struct FwdLate;

struct [[gsl::Pointer]] HolderLate {
  const char *keep;
  FwdLate &f; // expected-warning {{field 'f' uses more than one level of indirection}}
};

struct [[gsl::Pointer]] FwdLate {
  std::string_view sv;
};

// The pointer spelling of the same shape.
struct FwdLatePtr;

struct [[gsl::Pointer]] HolderLatePtr {
  FwdLatePtr *f; // expected-warning {{field 'f' uses more than one level of indirection}}
};

struct [[gsl::Pointer]] FwdLatePtr {
  std::string_view sv;
};

//===----------------------------------------------------------------------===//
// Referent completed BEFORE the holder: the order that was always reported, and
// must keep being reported identically.
//===----------------------------------------------------------------------===//

struct [[gsl::Pointer]] FwdEarly {
  std::string_view sv;
};

struct [[gsl::Pointer]] HolderEarly {
  const char *keep;
  FwdEarly &f; // expected-warning {{field 'f' uses more than one level of indirection}}
};

//===----------------------------------------------------------------------===//
// Must stay silent.
//===----------------------------------------------------------------------===//

// A single level of indirection is exactly what the model supports.
struct [[gsl::Pointer]] OneLevel {
  const char *p;
  std::string_view sv;
};

// A referent that holds no borrow adds no level to measure.
struct Plain {
  int a;
  double b;
};

struct [[gsl::Pointer]] HolderOfPlain {
  Plain &p;
};

// A referent never completed in this TU measures as depth 1 and is accepted:
// the multi-level path cannot be spelled without completing the type, so the TU
// that can express the hazard is one where the referent IS complete, and the
// check fires there instead.
struct NeverCompleted;

struct [[gsl::Pointer]] HolderOfOpaque {
  NeverCompleted *p; // no-warning
};
