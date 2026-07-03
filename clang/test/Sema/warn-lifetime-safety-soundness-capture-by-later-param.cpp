// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-const-subversion -verify %s
#include "Inputs/lifetime-analysis.h"

using std::string_view;

// Regression (crash on well-formed code): `[[clang::lifetime_capture_by(X)]]`
// naming a parameter X of a non-static member function crashed in
// handleLifetimeCaptureBy. The attribute numbers entities as this=0, params
// 1..N -- matching the modeled argument list, whose Args[0] is the implicit
// object -- but the fact generator dropped the object and reused the index,
// double-counting `this`; naming a parameter later than the annotated view then
// ran one past the end of the argument array (assertion / OOB read) at the call
// site. This must compile without crashing.
//
// -Wlifetime-safety-const-subversion enables the analysis (so fact generation --
// where the crash was -- runs) without turning on the diagnostics this code
// otherwise trips, keeping the regression focused on "does not crash".
// expected-no-diagnostics

struct View {
  string_view held;
  // 'dummy' is the last parameter (index 2: this=0, s=1, dummy=2).
  void set(string_view s [[clang::lifetime_capture_by(dummy)]], int dummy) {
    held = s;
  }
};

void call() {
  View v;
  string_view p;
  v.set(p, 0); // previously crashed in handleLifetimeCaptureBy
}
