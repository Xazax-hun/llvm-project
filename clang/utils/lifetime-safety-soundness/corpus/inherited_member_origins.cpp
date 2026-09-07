// DESC: the borrow-holding member lives in a BASE CLASS. `Box` is a [[gsl::Owner]] whose only
// borrow-holding member, `d`, is declared in a plain base -- and a store into it, plus the
// dangling read after the delete, went completely unreported. The identical member declared
// directly in `Box` is caught.
//
// CAUSE: the origin tree walked `RD->fields()`, so a base subobject's members got no origins at
// all. A base's members are members of the derived object too, so the borrow had nowhere to
// land and every downstream check -- expiry, invalidation, escape -- had nothing to see. The
// record-level "does this type have origins?" test walked the same list, so a derived owner
// whose only such member came from a base looked as though it held no borrows whatsoever.
//
// It was not about ACCESS: a private or protected member declared directly in a TU-local owner
// is tracked. Only where the member was DECLARED mattered, and whether the base was itself an
// owner made no difference.
//
// The field walk now includes the bases', asking isTrackedField about the record being expanded
// rather than the one declaring the field -- so an inherited non-public member of a TU-local
// owner is tracked exactly as a directly declared one is, while a library owner's members stay
// opaque either way.
// EXPECT-ASAN: heap-use-after-free
volatile int g_sink;
struct Base {
protected:
  const int *d = nullptr;
};
class [[gsl::Owner(int)]] Box : Base {
public:
  static void run() {
    int *h = new int(3);
    Box c;
    c.d = h;
    delete h;
    g_sink = *c.d;     // heap-use-after-free
  }
};
int main() { Box::run(); }
