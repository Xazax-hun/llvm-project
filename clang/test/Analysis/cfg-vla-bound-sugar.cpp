// RUN: %clang_analyze_cc1 -analyzer-checker=debug.DumpCFG -std=c++11 -Wno-vla-cxx-extension %s > %t 2>&1
// RUN: FileCheck --input-file=%t %s

// A variable-length array's size expression is evaluated where the type is
// written, so it belongs in the CFG -- it can have side effects. It was found
// by casting the variable's type to ArrayType, which sees nothing when the
// array sits behind type sugar, so `__typeof__(char[n])` left the expression
// out of the CFG entirely.

int side();

// The size expression is present, and the declaration follows it.
// CHECK-LABEL: void typeof_vla(int n)
// CHECK:        1: side
// CHECK-NEXT:   2: [B1.1] (ImplicitCastExpr, FunctionToPointerDecay, int (*)(void))
// CHECK-NEXT:   3: [B1.2]()
// CHECK-NEXT:   4: n
// CHECK-NEXT:   5: [B1.4] (ImplicitCastExpr, LValueToRValue, int)
// CHECK-NEXT:   6: [B1.3] + [B1.5]
// CHECK-NEXT:   7: typeof(char[side() + n]) arr;
void typeof_vla(int n) {
  __typeof__(char[side() + n]) arr;
  (void)arr;
}

// The plain spelling, for comparison: identical elements, no sugar involved.
// CHECK-LABEL: void plain_vla(int n)
// CHECK:        1: side
// CHECK-NEXT:   2: [B1.1] (ImplicitCastExpr, FunctionToPointerDecay, int (*)(void))
// CHECK-NEXT:   3: [B1.2]()
// CHECK-NEXT:   4: n
// CHECK-NEXT:   5: [B1.4] (ImplicitCastExpr, LValueToRValue, int)
// CHECK-NEXT:   6: [B1.3] + [B1.5]
// CHECK-NEXT:   7: char arr[side() + n];
void plain_vla(int n) {
  char arr[side() + n];
  (void)arr;
}

// A typedef evaluates its bound at the TYPE DECLARATION, once. Declaring a
// variable of that type must NOT evaluate it again: `side()` appears exactly
// once, before the typedef, and `T arr;` adds nothing. Looking through typedef
// sugar at the variable would duplicate the whole expression here.
// CHECK-LABEL: void typedef_vla(int n)
// CHECK:        1: side
// CHECK-NEXT:   2: [B1.1] (ImplicitCastExpr, FunctionToPointerDecay, int (*)(void))
// CHECK-NEXT:   3: [B1.2]()
// CHECK-NEXT:   4: n
// CHECK-NEXT:   5: [B1.4] (ImplicitCastExpr, LValueToRValue, int)
// CHECK-NEXT:   6: [B1.3] + [B1.5]
// CHECK-NEXT:   7: typedef char T[side() + n];
// CHECK-NEXT:   8: T arr;
// CHECK-NEXT:   9: arr
void typedef_vla(int n) {
  typedef char T[side() + n];
  T arr;
  (void)arr;
}
