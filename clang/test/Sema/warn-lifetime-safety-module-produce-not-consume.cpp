// The whole-translation-unit lifetime-safety sweeps must treat a module the way
// they treat a PCH: report where the module is produced, and do not re-report in
// every translation unit that imports it. Module declarations are
// `isFromASTFile`, exactly like a PCH's, so the same rule covers both.
//
// Producing the module reports the hazards. This already worked before the PCH
// fix -- only TU_Prefix returned early from ActOnEndOfTranslationUnit, and a
// C++20 module interface is TU_Complete.
//
// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -std=c++20 -Wlifetime-safety-soundness -I%S/Inputs \
// RUN:   -emit-module-interface -o %t/haz.pcm -verify=produce %t/haz.cppm
//
// Importing it does not repeat them. This DID regress before: every importer
// re-analyzed the module's declarations and reported all of them against itself.
// RUN: %clang_cc1 -std=c++20 -Wlifetime-safety-soundness -fsyntax-only \
// RUN:   -fmodule-file=haz=%t/haz.pcm -verify=consume %t/main.cpp

//--- haz.cppm
module;
#include "lifetime-analysis.h"
export module haz;

std::string h_mut = "a mutable global string";

// Reached only by LifetimeSafetyFileVarInitAnalysis.
struct [[gsl::Pointer]] Watch {
  std::string_view v;
};
Watch h_watch; // produce-warning {{borrows from a mutable global or static object}}
// produce-warning@+1 {{cannot track global variable 'h_watch' here}}
static int h_wire = (h_watch.v = h_mut, 0);

// Reached only by LifetimeSafetyDestructionOrderAnalysis.
struct HasDtor {
  ~HasDtor();
};
HasDtor h_obj; // produce-warning {{destructor is not known to be safe against static destruction order}}

//--- main.cpp
// consume-no-diagnostics
import haz;
int main() { return 0; }
