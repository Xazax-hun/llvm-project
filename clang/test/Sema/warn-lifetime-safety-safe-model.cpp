// RUN: %clang_cc1 -fsyntax-only -verify %s
// RUN: %clang_cc1 -fsyntax-only -fexperimental-lifetime-safety-tu-analysis -verify %s

// Demonstrates the "safe programming model": enabling the soundness warnings
// as errors over a region makes the analysis refuse to silently miss a lifetime
// mistake there. Outside the region the warnings are off by default, and a
// per-construct '#pragma clang diagnostic ignored' is the explicit opt-out. The
// second RUN line confirms the pragma-as-error opt-in is honored under
// translation-unit-wide analysis, where the checks run at TU end -- pragma
// regions are resolved by source location, not by when the analysis runs.

struct Holder {
  int *p;
}; // Unknown ownership: can hold a borrow but is unannotated.

// Outside the safe region: soundness warnings are off by default.
void outside_region() {
  Holder h; // no-warning
  (void)h;
}

#pragma clang diagnostic push
#pragma clang diagnostic error "-Wlifetime-safety-soundness"

// Inside the safe region: a construct the analysis cannot model is an error.
void inside_region() {
  Holder h; // expected-error {{type 'Holder' can hold a borrow but is annotated neither [[gsl::Owner]] nor [[gsl::Pointer]], so lifetime safety cannot track its ownership}}
  (void)h;
}

// Per-construct opt-out: explicitly silence the warning for one construct.
void opt_out() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-soundness"
  Holder h; // no-warning
#pragma clang diagnostic pop
  (void)h;
}

#pragma clang diagnostic pop

// Back to the default outside the region.
void after_region() {
  Holder h; // no-warning
  (void)h;
}
