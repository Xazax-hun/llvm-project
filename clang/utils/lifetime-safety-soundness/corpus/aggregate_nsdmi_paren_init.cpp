// DESC: a hazard in a default member initializer applied by C++20 PARENTHESIZED aggregate
// initialization. `CXXDefaultInitExpr::children()` is empty, so an initializer's body enters
// the CFG only where a walk descends into it explicitly -- and `Agg x(1)` had no case of its
// own, falling to the generic child walk, which sees the CXXDefaultInitExpr and cannot reach
// what is inside it.
//
// The control is one character away: `Agg x{1}` reports. Both spellings initialize an
// aggregate and fill the members the caller left out from their initializers.
// EXPECT-ASAN: heap-use-after-free

volatile int sink;

int *g_p = nullptr;

void hold(int v);

struct Agg {
  int a;
  int m = (g_p = new int(7), delete g_p, hold(*g_p), 0);
};

int main() {
  Agg x(1);
  return x.m;
}

void hold(int v) { sink = v; }
