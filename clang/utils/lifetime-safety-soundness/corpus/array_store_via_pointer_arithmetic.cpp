// DESC: a borrow of a local is stored into an array element via POINTER
// ARITHMETIC on the decayed array (`*(arr+1) = &local`) rather than a subscript
// (`arr[1] = &local`), then read back after the local dies. All array elements
// share one element-origin; the store was previously only routed there for a
// syntactic ArraySubscriptExpr, so the pointer-arithmetic form silently dropped
// the dangling borrow (the array's pre-seeded loans kept lost-loan quiet). Found
// by the 5th multi-agent bypass hunt (D1).
// EXPECT-ASAN: stack-use-after-scope
int valid = 7;
int sink;

void run() {
  int *arr[3] = {&valid, &valid, &valid};
  {
    int local = 99;
    *(arr + 1) = &local; // store via pointer arithmetic; local dies at block end
  }
  sink = *arr[1]; // read the dangling element -> use-after-scope
}

int main() {
  run();
  return sink;
}
