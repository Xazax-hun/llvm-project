// DESC: a non-allocating placement-new with EXTRA placement arguments (a tag /
// allocator state, e.g. `new (buf, Tag{}) T`) returns the buffer it is placed
// into, but the analysis only forwarded the placement buffer's loan for the
// single-placement-argument form. With 2+ placement args it minted a FRESH
// heap-allocation loan decoupled from the buffer, so freeing the buffer
// (`delete[] buf`) did not dangle a borrow into the placed object -> silent
// heap-use-after-free. The buffer loan is now forwarded for any non-allocating
// placement form (first placement parameter is `void*`).
// EXPECT-ASAN: heap-use-after-free
struct Tag {};
void *operator new(unsigned long, void *p [[clang::lifetimebound]], Tag) noexcept {
  return p;
}
struct T { int v; };
int main() {
  char *buf = new char[sizeof(T)];
  T *p = new (buf, Tag{}) T{42}; // placed into buf
  int *q = &p->v;                // borrow into the placed object
  delete[] buf;                  // frees the backing storage
  return *q;                     // reads freed memory
}
