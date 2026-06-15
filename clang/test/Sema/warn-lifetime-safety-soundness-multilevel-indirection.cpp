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
  int **pp = &p;      // expected-warning {{local variable 'pp' uses more than one level of indirection}} expected-warning {{uses more than one level of indirection}}
  View *vp = nullptr; // expected-warning {{local variable 'vp' uses more than one level of indirection}}
  (void)p;
  (void)pp;
  (void)vp;
}

// Taking the address of an indirection forms a second level even when no
// declaration captures it; flagged at the expression.
void addressof_indirection(int *p, View v) {
  (void)&p; // expected-warning {{uses more than one level of indirection}}
  (void)&v; // expected-warning {{uses more than one level of indirection}}
}

// Taking the address of a non-indirection (a scalar, an owner) stays a single
// level and is fine.
void addressof_value(int x) {
  (void)&x; // no-warning
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

// The RETURN TYPE is subject to the same rule: returning a reference/pointer to
// an indirection (a view, a pointer) is a second level. Storing through such a
// returned reference (`obj.ref() = borrow;`) would otherwise silently drop the
// borrow.
struct Box {
  View v;
  View &ref() [[clang::lifetimebound]] { return v; } // expected-warning {{the return type of 'ref' uses more than one level of indirection}}
  View *ptr() [[clang::lifetimebound]] { return &v; } // expected-warning {{the return type of 'ptr' uses more than one level of indirection}} expected-warning {{uses more than one level of indirection}}
  View by_value() { return v; }                       // no-warning: a view returned by value is a single level
};

int **returns_pp() {       // expected-warning {{the return type of 'returns_pp' uses more than one level of indirection}}
  return nullptr;
}

// Single-level returns are fine.
int *returns_p(int *p) { return p; }    // no-warning
View returns_view(View v) { return v; } // no-warning
int returns_int() { return 0; }         // no-warning
