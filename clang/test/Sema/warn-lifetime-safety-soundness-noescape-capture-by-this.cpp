// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-noescape -verify %s

#include "Inputs/lifetime-analysis.h"
using std::string_view;

// A [[clang::noescape]] parameter must not be captured into the object. When it
// is forwarded into a callee's [[clang::lifetime_capture_by(this)]] parameter,
// the borrow becomes reachable from the (caller-scoped) object -- the noescape
// promise is a lie. The capture flows into the whole-object `this` origin, which
// never reaches a return/field/global escape point, so this was missed.

struct [[gsl::Pointer(char)]] H {
  string_view sv;
  void capture(string_view in [[clang::lifetime_capture_by(this)]]) { sv = in; }

  // Forwarding a noescape parameter into capture_by(this) escapes into `this`.
  void outer(string_view x [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
    capture(x); // expected-note {{param returned here}}
  }

  // Control: a non-noescape (capture_by) parameter forwarded the same way is the
  // documented, intended capture -- no violation.
  void outer_ok(string_view x [[clang::lifetime_capture_by(this)]]) {
    capture(x); // no-warning
  }
};

// Control (false-positive guard): a noescape parameter captured into a *local*
// receiver does not escape the function (the local object dies at return), so
// no noescape violation.
void capture_into_local(string_view x [[clang::noescape]]) {
  H local;
  local.capture(x); // no noescape violation
}
