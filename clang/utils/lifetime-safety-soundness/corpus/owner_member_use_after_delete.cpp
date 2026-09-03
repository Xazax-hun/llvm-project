// DESC: an owner's private member caches a borrow of heap storage that is then deleted. This
// is the case NO store-site rule can reach: at the store, "the owner adopts this buffer" and
// "the owner caches a borrow of heap someone else frees" are indistinguishable, so judging the
// store either misses this or rejects every owner implementation. It falls out for free once
// the member has an origin -- the delete invalidates the loan the member holds and the later
// read is an ordinary use-after-free. naked-delete does not cover it either: the allocation
// was seen, so the delete is not naked.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
volatile int g_sink;

class [[gsl::Owner(int)]] Box {
  const int *d = nullptr;
public:
  static void run() {
    int *h = new int(3);
    Box c;
    c.d = h;
    delete h;
    g_sink = *c.d;          // UAF
  }
};

int main() { Box::run(); return 0; }
