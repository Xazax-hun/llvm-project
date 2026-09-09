// DESC: a structured binding whose container is mutated inside a LOOP. An origin
// mentioned in only one CFG block is kept block-local and its loans are dropped at
// the boundary; computePersistentOrigins decides that by walking the facts and
// registering the origins each names -- and InvalidateOrigin did not register the
// origin it invalidates.
//
// It usually got away with it because the receiver is re-issued in the block that
// mutates it. A structured binding expands every use to the SAME MemberExpr, so one
// origin carries the member for the whole function; with the mutation in a loop, the
// invalidation names an origin projected in an earlier block, sees no loans, and
// reports nothing. The identical loop written `rec.samples` re-projects inside the
// loop body and was reported all along, as was the binding with the push in the same
// block -- so neither the binding nor the loop alone was enough.
//
// Projection, FieldStore and ArgumentOverlap were missing from the same switch.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <vector>

volatile int g_sink;

struct Record {
  std::string name;
  std::vector<int> samples;
};

int main() {
  Record rec{"sensor", {1, 2, 3}};
  auto &[name, samples] = rec;

  int *first = &samples[0]; // borrow into the heap buffer
  for (int i = 0; i < 200; ++i)
    samples.push_back(i); // reallocates; `first` dangles
  (void)name.size();

  g_sink = *first;
  return 0;
}
