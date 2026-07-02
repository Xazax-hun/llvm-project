// DESC: a stack address flows through unary plus (`+&local`, the pointer
// identity) into a pointer, on one arm of a ternary whose other arm holds a
// valid persistent loan (&g). UO_Plus used to fall through the fact generator's
// unary-operator handling, leaving the result origin empty; the union merge with
// the valid loan then masked the empty-origin lost-loan backstop, so the dangle
// escaped clean. Unary plus on a pointer must carry the operand's loan so the
// borrowed local's expiry is caught (use-after-scope) across the merge.
// EXPECT-ASAN: stack-use-after-scope
static volatile int sink;
static int g = 0;

int main(int argc, char **) {
  int *p = &g;
  {
    int local = 1;
    p = (argc > 100) ? p : +&local; // +&local dangles when the false arm is taken
  }                                 // local goes out of scope
  *p = 42; // stack-use-after-scope
  sink = *p;
  return 0;
}
