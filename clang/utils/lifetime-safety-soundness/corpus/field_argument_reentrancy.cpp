// DESC: reentrancy through a FIELD-typed argument. The same shape as
// virtual_param_reentrancy_base_upcast, except the argument is a member rather
// than `*this`, which is what made it slip through: confirming the hazard asked
// only which RECORD a loan denoted, and a member access widened to its enclosing
// object, so `w.C`'s loan looked like a loan of `w`. Accepting that would have
// flagged every disjoint sibling of the same type, so the enclosing-object
// fallback was refused for this case and the real bug went unreported. With
// field-sensitive access paths the borrow denotes `w.C.Text` and the argument
// denotes `w.C`: one is a prefix of the other, so the mutation provably reaches
// the borrow, while a sibling `w.B` diverges and stays clean.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Comp {
  virtual ~Comp() = default;
  virtual void reload() = 0;
};

// Truthful annotation: C genuinely does not escape.
static void notifyAll(Comp &C [[clang::noescape]]) { C.reload(); }

struct Cfg : Comp {
  std::string Text = "a configuration blob string long enough to be heap alloc";
  void reload() override { Text = std::string(400, 'z'); } // reallocates Text
};

struct Wrapper {
  Cfg C;
};

int main() {
  Wrapper W;
  std::string_view V = W.C.Text; // borrow into W.C.Text
  notifyAll(W.C);                // dispatches back and reallocates it
  sink = V[0];                   // heap-use-after-free
  return 0;
}
