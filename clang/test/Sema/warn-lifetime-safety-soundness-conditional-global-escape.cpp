// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-soundness -verify %s

// A stack address stored into a global must be diagnosed even when the store is
// CONDITIONAL (on some-but-not-all paths) or inside a loop. The escaping origin
// (the global) is defined in the storing block and only read at the function
// exit block via an escape fact; it must participate in dataflow joins so its
// loan survives the merge to reach the expiry check. (Regression: the global's
// origin was misclassified as block-local because the escape fact was not
// counted as an appearance, dropping its loan at the join -> silent.)

int *g_p; // expected-note 3 {{this global dangles}}

void conditional(int c) {
  int local = 7;
  if (c)
    g_p = &local; // expected-warning {{stack memory associated with local variable 'local' escapes to the global variable 'g_p' which will dangle}}
}

void in_loop(int n) {
  for (int i = 0; i < n; ++i) {
    int local = i;
    g_p = &local; // expected-warning {{stack memory associated with local variable 'local' escapes to the global variable 'g_p' which will dangle}}
  }
}

void unconditional() {
  int local = 7;
  g_p = &local; // expected-warning {{stack memory associated with local variable 'local' escapes to the global variable 'g_p' which will dangle}}
}

// Negative: a conditional store that never leaks a stack address is silent.
void ok(int c) {
  int local = 7;
  if (c)
    g_p = nullptr; // no-warning
  (void)local;
}
