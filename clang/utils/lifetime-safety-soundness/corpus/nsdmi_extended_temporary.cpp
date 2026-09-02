// DESC: a temporary bound to a reference member by a DEFAULT MEMBER INITIALIZER is
// lifetime-extended to the enclosing object -- the AST records it as "extended by
// Var 'a'" -- so it dies with `a`. That expiry is found by walking the variable's
// initializer for extended temporaries, and the walk descended through
// Stmt::children(). But a default initializer holds its expression on the FIELD, so
// CXXDefaultInitExpr::children() is an empty range BY CONSTRUCTION and the walk never
// entered it. The temporary was therefore never collected and never expired: its loan
// was tracked correctly all the way into `out` and then simply never died, so the use
// after the block was invisible. Zero diagnostics of any kind, not a refusal. Neither
// -Wdangling nor -Weverything says anything about this either.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

static std::string mk() {
  return std::string("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
}

struct [[gsl::Pointer(char)]] Agg { const std::string &r = mk(); };

int main() {
  std::string_view out;
  { Agg a{}; out = a.r; } // temporary extended to `a`, dies with `a`
  sink = out[0];          // heap-use-after-free
  return 0;
}
