// RUN: %clang_cc1 -fsyntax-only -std=c++20 -fblocks -Wlifetime-safety-soundness -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -fblocks -Wlifetime-safety-soundness -fexperimental-lifetime-safety-tu-analysis -verify %s

// TU-end analysis (-fexperimental-lifetime-safety-tu-analysis) discovers the
// functions to analyze via a call-graph post-order walk plus a supplementary
// sweep. The call graph tracks only FunctionDecls -- never Objective-C methods
// or blocks -- so an Objective-C method body (and a block body) was never
// analyzed in TU mode: a silent coverage gap. The supplementary sweep now
// analyzes Objective-C methods and blocks too, so TU mode matches the default
// per-function mode. Both RUN lines must produce identical diagnostics.

__attribute__((objc_root_class))
@interface Compute
- (int)run;
- (int)ok;
@end

@implementation Compute
// A use-after-scope inside an Objective-C method body: 'q' borrows the
// inner-scope local 'b', read after that scope ends. This was silent in TU mode.
- (int)run {
  int *q = nullptr;
  {
    int b = 0xB;
    q = &b; // expected-warning {{local variable 'b' does not live long enough}}
  }         // expected-note {{destroyed here}}
  return *q; // expected-note {{later used here}}
}

// A clean method stays silent.
- (int)ok {
  int x = 1;
  int *p = &x;
  return *p; // no-warning
}
@end

// A block body is analyzed too (the call graph also never tracked blocks).
typedef int (^IntBlock)(void);
IntBlock make_block() {
  return ^{
    int *q = nullptr;
    {
      int b = 0xB;
      q = &b; // expected-warning {{local variable 'b' does not live long enough}}
    }         // expected-note {{destroyed here}}
    return *q; // expected-note {{later used here}}
  };
}
