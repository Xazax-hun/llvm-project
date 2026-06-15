// DESC: aggregate initialization of a [[gsl::Pointer]] view from its underlying
// pointer (`v = View{heap}`) was unmodeled -- the InitListExpr origin stayed
// empty so the captured borrow was dropped. The destination `this->v` already
// held the method's `this` caller-scope placeholder loan (which never expires
// intra-procedurally), masking the lost-loan backstop, so freeing `heap` and
// then reading `v.p` was a silent use-after-free. The aggregate init now merges
// the initializer's loans into the view's own origin, so the delete invalidates
// the borrow the view carries.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
struct [[gsl::Pointer]] View {
  const char *p;
};

struct [[gsl::Pointer]] Holder {
  View v;
  __attribute__((noinline)) char leak() {
    char *heap = new char[64];
    for (int i = 0; i < 63; i++)
      heap[i] = 'A' + (i % 26);
    heap[63] = 0;
    v = View{heap}; // aggregate member-store: borrow captured into the view
    delete[] heap;  // heap freed -> this->v.p dangles
    return v.p[0];  // use-after-free
  }
};

volatile char sink;

int main() {
  sink = Holder{}.leak();
  return 0;
}
