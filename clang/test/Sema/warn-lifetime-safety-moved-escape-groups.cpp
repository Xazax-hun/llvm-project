// -Wlifetime-safety-dangling-global-moved was in no parent group at all, so it
// could only ever be enabled by naming it directly -- not even
// -Wlifetime-safety-all reached it. Its sibling
// -Wlifetime-safety-dangling-field-moved is in -Wlifetime-safety-strict, which
// is what the omission is measured against. Neither variant had any test, which
// is why the orphan went unnoticed.
//
// The first RUN line is the regression: the group must be reachable from
// -Wlifetime-safety-strict, and hence from -Wlifetime-safety-soundness and
// -Wlifetime-safety-all, which both contain strict. The second pins the
// boundary: -Wlifetime-safety-permissive deliberately excludes the moved
// variants, so naming it must stay silent.
//
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-dangling-global-moved -Wno-dangling -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-strict -Wno-lifetime-safety-invalidation -Wno-lifetime-safety-assumed-invalidation -Wno-dangling -verify %s
// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-permissive -Wno-dangling -verify=nomoved %s

#include "Inputs/lifetime-analysis.h"

// nomoved-no-diagnostics

std::string_view g_sv; // expected-note {{this global dangles}}

// A borrow of a local that is moved-from, escaping to global storage. The report
// anchors at the borrow's creation and names the move, which is what
// distinguishes this group from the non-moved -Wlifetime-safety-dangling-global.
void escapes_to_global_after_move() {
  std::string s;
  std::string_view sv = s; // expected-warning {{may escape to the global variable 'g_sv' which will dangle}}
  std::string t = std::move(s); // expected-note {{potentially moved here}}
  g_sv = sv;
}
