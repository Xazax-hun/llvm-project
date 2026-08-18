// DESC: a hazard written wholly inside a default member initializer reached through
// AGGREGATE initialization. A default member initializer is code, and through aggregate
// initialization there is no constructor to carry it: the CXXDefaultInitExpr sits inline in
// the enclosing function, and its subexpression enters the CFG only when
// CFGBuildOptions::AddCXXDefaultInitExprInAggregates is set -- which, of the analyses that
// build a CFG, only the static analyzer was doing. So nothing inside the initializer was
// ever handed to the analysis: not refused, simply invisible.
//
// The controls localize it exactly: `G g;` instead of `G g{}`, or giving `G` a `G(){}` or
// `G() = default;`, all report. Only the aggregate form was blind. It lost on-model borrow
// findings too, not just this raw new/delete shape -- the same class holding a
// `std::vector<std::string>` plus a `std::string_view` member lost both an invalidation and
// a self-referential report when written as an aggregate.
// EXPECT-ASAN: heap-use-after-free

volatile int sink;

int *g_p = nullptr;

void hold(int v);

// No constructor, so `G g{}` initializes it as an aggregate and the initializer below runs
// with nothing analyzing it.
struct G {
  int m = (g_p = new int(7), delete g_p, hold(*g_p), 0);
};

int main() {
  G g{};
  return g.m;
}

void hold(int v) { sink = v; }
