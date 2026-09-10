// A `#pragma clang diagnostic` region must enable the whole-translation-unit
// lifetime-safety sweeps, not just the per-function path. Both RUN lines below
// must report the same hazards; only how the warning is switched on differs.
//
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -DVIA_PRAGMA %s 2>&1 | FileCheck %s

#include "Inputs/lifetime-analysis.h"

#ifdef VIA_PRAGMA
#pragma clang diagnostic push
#pragma clang diagnostic warning "-Wlifetime-safety-soundness"
#endif

std::string g_mut = "a mutable global string";

// Reported only by LifetimeSafetyFileVarInitAnalysis: a namespace-scope
// initializer is not a function body, so no per-function analysis reaches it.
struct [[gsl::Pointer]] Watch {
  std::string_view v;
};
Watch g_watch;
static int g_wire = (g_watch.v = g_mut, 0);
// CHECK-DAG: borrows from a mutable global or static object

// Reported only by LifetimeSafetyDestructionOrderAnalysis. No function anywhere
// here, so nothing else can produce it.
struct HasDtor {
  ~HasDtor();
};
HasDtor g_obj;
// CHECK-DAG: whose destructor is not known to be safe against static destruction order

#ifdef VIA_PRAGMA
#pragma clang diagnostic pop
#endif
