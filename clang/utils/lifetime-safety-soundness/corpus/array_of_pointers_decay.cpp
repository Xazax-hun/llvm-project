// DESC: a store through a dereference of a conditionally-selected array of
// pointers (`*(c ? arr : arr2) = &local`) was not routed to either array's
// element-origin, so the borrow of `local` was dropped and the later read of
// `*arr[0]` dangled silently. An array whose element type is an indirection
// decays to a pointer-to-pointer (a double indirection the analysis cannot
// model); such a decay -- other than as an `arr[i]` subscript base -- is now
// rejected.
// EXPECT-ASAN: stack-use-after-scope
volatile int sink;

int run(bool c) {
  int *arr[2];
  int *arr2[2];
  {
    int local = 2;
    *(c ? arr : arr2) = &local; // decays arr/arr2 to int** -> double indirection
  }                             // local dies
  return *arr[0];               // use-after-scope
}

int main(int argc, char **) {
  sink = run(argc > 0);
  return sink;
}
