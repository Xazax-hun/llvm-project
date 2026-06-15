// DESC: an assignment whose destination is spelled with a value-preserving
// explicit reference cast -- `static_cast<int*&>(p) = &local` (or the C-style
// `(int*&)p = ...`) -- routed to no tracked origin: handleAssignment stripped
// only parens and IMPLICIT casts, so the explicit reference cast matched none of
// the handled LHS forms and the store was dropped. A prior valid loan in `p`
// kept its origin non-empty, masking lost-loan on the read. The LHS routing now
// looks through value-preserving (CK_NoOp) explicit reference casts.
// EXPECT-ASAN: heap-use-after-free
volatile int sink;

int main() {
  int *p = nullptr;
  int valid = 7;
  p = &valid; // prior valid loan (masks lost-loan)
  int *heap = new int(99);
  static_cast<int *&>(p) = heap; // store through an explicit reference cast
  delete heap;
  sink = *p; // heap-use-after-free
  return 0;
}
