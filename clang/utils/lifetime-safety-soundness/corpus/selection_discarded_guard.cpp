// DESC: a scope guard discarded through an expression that SELECTS rather than
// consumes. Deciding which temporaries the CFGTemporaryDtor handler owns was done
// by enumerating the contexts that discard a value, and an unrecognized parent was
// assumed to consume -- so every missing entry was a SILENT miss. A conditional
// operator consumes neither arm, it selects one, so each arm is discarded exactly
// when the conditional is; `__builtin_choose_expr` and `_Generic` select at compile
// time and behave the same. The rule is now stated as who ELSE models the
// temporary (a materialized one belongs to the full-expression cleanup, one handed
// to a call belongs to the call), with modelling as the fall-through -- so a
// missing wrapper now costs a duplicate diagnostic instead of a dropped one.
// FLAGS: -Wno-unused-value
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile char g_sink = 0;

struct [[gsl::Pointer]] Grower {
  std::vector<int> *v;
  ~Grower() { v->resize(4000); } // reallocates the borrowed vector
};

int main() {
  std::vector<int> vec(1, 5);
  int *p = &vec[0];
  true ? Grower{&vec} : Grower{&vec}; // both temporaries destroyed here
  g_sink = (char)*p;                  // heap-use-after-free
  return 0;
}
