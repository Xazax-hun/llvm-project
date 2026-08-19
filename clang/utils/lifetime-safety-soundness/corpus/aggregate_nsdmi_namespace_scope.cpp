// DESC: a hazard in a default member initializer of a NAMESPACE-SCOPE aggregate. The sweep
// over dynamic initializers is the only entry point that reaches such a declaration -- a
// file-scope variable is neither a function scope nor a call-graph node -- and it was not
// asking the CFG for default member initializer bodies, though the three other entry points
// all were. So the same aggregate was covered as a local and blind at namespace scope.
//
// That sweep also serves the default-argument analysis, so the omission covered both.
// EXPECT-ASAN: heap-use-after-free

volatile int sink;

int *g_p = nullptr;

void hold(int v);

struct Agg {
  int m = (g_p = new int(7), delete g_p, hold(*g_p), 0);
};

Agg g{};

int main() { return g.m; }

void hold(int v) { sink = v; }
