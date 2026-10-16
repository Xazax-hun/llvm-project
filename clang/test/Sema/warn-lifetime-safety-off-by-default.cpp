// RUN: %clang_cc1 -fsyntax-only -std=c++20 -mllvm -debug-only=LifetimeFacts %s 2>&1 | FileCheck --check-prefix=OFF --allow-empty %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-use-after-scope -mllvm -debug-only=LifetimeFacts %s 2>&1 | FileCheck --check-prefix=ON %s
// REQUIRES: asserts

// The analysis must not run when no lifetime-safety diagnostic is enabled. The
// gate is derived from -Wlifetime-safety-all, which also pulls in an upstream
// warning that is on by default (-Wmissing-noescape); counting that one made
// the analysis run on every function of every compile.

// OFF-NOT: Lifetime Analysis Facts
// ON: Lifetime Analysis Facts

int *p;
void f() {
  {
    int x;
    p = &x;
  }
  *p = 1;
}
