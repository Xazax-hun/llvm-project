// DESC: an array-element store spelled `(*&arr)[i] = p` -- a dereference of the
// address of the array -- was not routed to the array's shared element-origin
// (the `*&arr` round-trip built a fresh, disconnected origin), so the stored
// borrow never entered the element and the read saw only the non-expiring
// "uninitialized" sentinel: no use-after-free, no lost-loan. `*&E` is now mapped
// to `E`, so the store routes like a plain `arr[i]` store.
// EXPECT-ASAN: heap-use-after-free
volatile int sink;

int main() {
  int *arr[4];
  int *heap = new int(55);
  (*&arr)[0] = heap; // store through the *&arr round-trip
  delete heap;
  sink = *arr[0]; // heap-use-after-free
  return 0;
}
