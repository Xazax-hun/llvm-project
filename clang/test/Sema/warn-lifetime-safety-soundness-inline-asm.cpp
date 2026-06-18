// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// Inline assembly is opaque to the analysis: an output operand can reseat a
// pointer to anything (so a stale loan on it would be wrongly trusted), and an
// input or memory-clobbering operand can move or invalidate a borrow, with no
// modeled flow. It is rejected under the safe programming model.
//
// This closes a bypass: an asm output operand reseating a pointer was unmodeled,
// so the pointer kept its prior (stale) loan -- masking lost-loan -- while it
// actually pointed at a now-dead local.

int g_sink;
int g_valid;

void asm_output_reseat() {
  int *p = &g_valid; // a prior valid loan would otherwise mask the reseat
  {
    int local = 42;
    asm("mov %0, %1" : "=r"(p) : "r"(&local)); // expected-warning {{inline assembly is not modeled by lifetime safety analysis}}
  }
  g_sink = *p; // really reads the dead 'local'
}

void asm_input_borrow() {
  int local = 7;
  asm("" : : "r"(&local)); // expected-warning {{inline assembly is not modeled by lifetime safety analysis}}
}

// A memory-clobbering barrier is rejected too (it can invalidate borrows).
void asm_barrier() {
  asm volatile("" : : : "memory"); // expected-warning {{inline assembly is not modeled by lifetime safety analysis}}
}

//===----------------------------------------------------------------------===//
// Negative: a function with no inline assembly is unaffected.
//===----------------------------------------------------------------------===//
int no_asm(int x) {
  int *p = &x;
  return *p; // no-warning
}
