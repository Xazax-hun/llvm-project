// Hazards that only the whole-translation-unit lifetime-safety sweeps can find
// must be reported where the code is written -- i.e. while a PCH is produced --
// and not again in every translation unit that consumes the PCH.
//
// RUN: rm -rf %t && split-file %s %t
//
// Producing the PCH reports them. Before, ActOnEndOfTranslationUnit returned
// early for a translation unit prefix and the sweeps never ran here.
// RUN: %clang_cc1 -std=c++20 -Wlifetime-safety-soundness -x c++-header \
// RUN:   -I%S/Inputs -emit-pch -o %t/haz.pch -verify=produce %t/haz.h
//
// Consuming it does not repeat them: the sweeps look only at declarations of
// the current translation unit. Before, each consumer re-reported all of them.
// RUN: %clang_cc1 -std=c++20 -Wlifetime-safety-soundness -fsyntax-only \
// RUN:   -include-pch %t/haz.pch -verify=consume %t/main.cpp
//
// Without a PCH the same hazards are still found, so nothing is lost overall.
// RUN: %clang_cc1 -std=c++20 -Wlifetime-safety-soundness -fsyntax-only \
// RUN:   -I%t -I%S/Inputs -verify=produce %t/direct.cpp

//--- haz.h
#include "lifetime-analysis.h"

std::string h_mut = "a mutable global string";

// Reached only by LifetimeSafetyFileVarInitAnalysis: a namespace-scope
// initializer is not a function body.
struct [[gsl::Pointer]] Watch {
  std::string_view v;
};
Watch h_watch; // produce-warning {{borrows from a mutable global or static object}}
// produce-warning@+1 {{cannot track global variable 'h_watch' here}}
static int h_wire = (h_watch.v = h_mut, 0);

// Reached only by LifetimeSafetyDestructionOrderAnalysis. No function involved.
struct HasDtor {
  ~HasDtor();
};
HasDtor h_obj; // produce-warning {{destructor is not known to be safe against static destruction order}}

//--- main.cpp
// consume-no-diagnostics
int main() { return 0; }

//--- direct.cpp
// The expectations live in haz.h and must all fire here too.
#include "haz.h"
int main() { return 0; }
