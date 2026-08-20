// DESC: a subscript whose INDEX frees what the base borrows. Subscripting reads the
// BASE in order to follow it, exactly as a dereference does -- but nothing modelled
// that read, so the borrow was not live at the deallocation and `p[(delete p, 0)]`
// went unreported, while the dereference spelling `*(delete p, p)` was caught. The
// same gap covered a WRITE through the subscript and the invalidation flavour,
// where the index reallocates the container the base borrows.
//
// The read is recorded as an IMPLICIT use: it is part of the subscript rather than
// a use the author wrote separately, and the base carries the same loans as the
// object being reported, so an explicit use double-reported every ordinary
// `sv.data()[0]` under the lost-loan and borrow-from-global checks -- both of which
// skip implicit uses, while expiry, invalidation and use-after-free all have an
// implicit-use reporting path.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
volatile int sink;

int main() {
  int *p = new int(7);
  sink = p[(delete p, 0)]; // the index frees p, then p is read anyway
  return 0;
}
