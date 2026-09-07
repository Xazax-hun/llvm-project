// DESC: the temporary in the common operand of a GNU binary conditional (`a ?: b`) never
// expires. `makeStr().c_str() ?: "default"` leaves the caller holding a pointer into a
// std::string that died at the end of the full expression -- and deleting just the
// `?: "default"` makes the same line report.
//
// CAUSE: the common operand is evaluated once, and every accessor for it -- getCond(),
// getTrueExpr() -- hands back the OpaqueValueExpr that STANDS FOR it rather than the operand
// itself. The CFG's temporary-destructor pass does not descend through an opaque value, so the
// temporary gets no cleanup element and no destructor. Expiry is the analysis's single trigger
// for reporting a use after a temporary dies, so with none emitted the borrow looks immortal
// and use-after-scope, return-stack-addr and dangling-global went quiet together, with no
// refusal marking the gap.
//
// The construct is now REFUSED rather than modeled. Fixing the CFG is the better answer, but
// the obvious attempt is wrong: visiting the common operand in
// VisitConditionalOperatorForTemporaries double-counts whenever the temporary is already
// reachable, which it is when the conditional is class-typed -- `A a = A() ?: A();` then
// destroys the common operand's temporary twice, one extra destructor in the join block on
// every path.
//
// `?:` is a GNU extension, so the refusal costs little; a `?:` whose common operand creates no
// temporary is unaffected.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

// Returns a fresh string; the caller gets a temporary.
static std::string makeStr() { return std::string(4096, 'x'); }

int main() {
  // The temporary dies at the end of this statement, but `s` keeps pointing into it.
  const char *s = makeStr().c_str() ?: "default";
  sink = s[0];
  return 0;
}
