// DESC: a borrow forwarded through a GNU statement expression (`g = cond ?
// keep : ({ side(); p; })`) was silently dropped: the statement-expression's
// value origin carried no loan, so the `?:` merge with the valid `keep` arm
// masked the lost-loan backstop. A statement expression now forwards its final
// expression's origin and marks it used at the statement-expression's program
// point, so the borrow is tracked.
// FLAGS: -Wno-gnu-statement-expression
// EXPECT-ASAN: heap-use-after-free
int side() { return 0; }

volatile int sink;

void capture(bool cond) {
  int *keep = new int(1);
  int *g = nullptr;
  {
    int *heap = new int(42);
    int *p = heap;
    g = cond ? keep : ({ side(); p; }); // stmt-expr forwards p's borrow
    delete heap;                        // on the false path, g dangles
  }
  sink = *g; // heap-use-after-free
}

int main() {
  capture(false);
  return sink;
}
