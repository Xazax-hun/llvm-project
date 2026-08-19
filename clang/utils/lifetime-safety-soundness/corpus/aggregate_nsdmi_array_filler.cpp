// DESC: a hazard in a default member initializer reached through an ARRAY FILLER. An
// initializer list holds one filler expression standing for every element the caller did not
// write out, and it is not among the list's children -- so the walk over children never saw
// it and the initializer's body was in no CFG.
//
// The control writes the elements out: `Agg arr[2] = {{}, {}}` reports for each, while
// `Agg arr[2] = {}` reported nothing.
//
// The filler must be descended into WITHOUT being added to the CFG itself: it is an implicit
// node, and adding it produced an unmodeled-expression diagnostic with no source location,
// which can be neither located nor suppressed.
// EXPECT-ASAN: heap-use-after-free

volatile int sink;

int *g_p = nullptr;

void hold(int v);

struct Agg {
  int m = (g_p = new int(7), delete g_p, hold(*g_p), 0);
};

int main() {
  Agg arr[2] = {};
  return arr[0].m;
}

void hold(int v) { sink = v; }
