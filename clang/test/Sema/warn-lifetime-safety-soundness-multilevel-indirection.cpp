// RUN: %clang_cc1 -fsyntax-only -Wlifetime-safety-multilevel-indirection -verify %s

// The "safe programming model" supports only a single level of indirection.
// Declarations with more than one level (e.g. 'int **', 'int *&', a pointer to
// a gsl::Pointer) cannot be fully modeled and are flagged.

struct [[gsl::Pointer]] View {
  const int *p;
};

// Single level of indirection (raw pointer, reference, view): no warning.
void single_level(int *p, int &r, View v) {
  int *lp = p;
  (void)lp;
  (void)r;
  (void)v;
}

void params(int **pp,   // expected-warning {{parameter 'pp' uses more than one level of indirection, which lifetime safety analysis cannot fully model; the safe programming model supports only a single level of indirection}}
            int *&rp) { // expected-warning {{parameter 'rp' uses more than one level of indirection}}
  (void)pp;
  (void)rp;
}

void locals() {
  int x = 0;
  int *p = &x;        // single level: no warning
  int **pp = &p;      // expected-warning {{local variable 'pp' uses more than one level of indirection}}
  View *vp = nullptr; // expected-warning {{local variable 'vp' uses more than one level of indirection}}
  (void)p;
  (void)pp;
  (void)vp;
}

// main's 'argv'/'envp' are 'char**' (a character pointer-chain) by language
// mandate, so they are exempt from the single-indirection rule -- using them
// (e.g. 'argv[0][0]') is fine too. The same type in any other function is still
// flagged.
int main(int argc, char **argv, char **envp) { // no-warning
  return argc + argv[0][0] + envp[0][0];
}

void not_main(char **argv) { // expected-warning {{parameter 'argv' uses more than one level of indirection}}
  (void)argv;
}
