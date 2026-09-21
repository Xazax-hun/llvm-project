// DESC: a borrow of dead stack storage, dereferenced, where the report was lost to
// liveness bookkeeping rather than to anything about the borrow. The analysis asks
// "can this pointer still be FOLLOWED from here?" so that a harmless use of a
// dangling pointer (`++p` computes an address and touches nothing) is not reported.
// That answer accumulates over every use forward of the point where the storage
// died -- and the origin-flow transfer OVERWROTE the accumulated answer for the
// flow's SOURCE with the destination's.
//
// In a backward analysis the flow is visited AFTER the later use it must not
// forget. Here `e`'s only use is `++e`, so `e` answers "cannot be followed"; the
// flow `b -> e` then stamped that onto `b`, discarding the `*b` below. Remove the
// two middle lines and the same function reports.
//
// The fix merges instead of replacing: the source keeps what it had and gains what
// the destination can do with the value. Note the escape transfer sets the same
// bits to true unconditionally, so this overwrite could erase those too -- storing
// the dangling `b` into a global or returning it was equally silent.
// EXPECT-ASAN: stack-use-after-scope

volatile int sink;

int main() {
  int *b = nullptr;
  {
    int arr[4]{1, 2, 3, 4};
    b = arr; // borrow of storage that dies at the closing brace
  }
  int *e = b + 1; // flow b -> e
  ++e;            // e's only use: retargets, so e alone need not be reported
  sink = *b;      // stack-use-after-scope
  return 0;
}
