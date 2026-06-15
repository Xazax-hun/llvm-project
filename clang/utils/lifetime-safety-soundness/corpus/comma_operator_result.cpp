// DESC: a borrow used via a COMMA operator result (`g = cond ? keep : (f(), p)`)
// was silently dropped: VisitBinaryOperator had no BO_Comma case, so the comma
// result origin carried no loan. The `?:` then merged the valid `keep` loan with
// the empty comma origin, leaving a non-empty set that masked lost-loan -- so the
// dangling `?:`-false-arm pointer escaped undetected. The comma now forwards its
// right operand's loans.
// EXPECT-ASAN: heap-use-after-free
int side() { return 0; }

volatile int sink;

void capture(bool cond) {
  int *keep = new int(1);
  int *g = nullptr;
  {
    int *heap = new int(42);
    int *p = heap;
    g = cond ? keep : (side(), p); // false arm: comma result carries p's borrow
    delete heap;                   // on the false path, g dangles
  }
  sink = *g; // heap-use-after-free
}

int main() {
  capture(false);
  return sink;
}
