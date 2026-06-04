// DESC: a const method invalidates a borrow by deleting through a pointer member
// EXPECT-ASAN: heap-use-after-free
struct [[gsl::Owner(int)]] Buffer {
  int *data;
  Buffer() : data(new int[1]) { data[0] = 42; }
  ~Buffer() { delete[] data; }
  int *get() const [[clang::lifetimebound]] { return data; }
  // const, no mutable/const_cast: deleting through a const pointer member is
  // allowed, yet it invalidates borrows. Flagged by the naked-delete check.
  void clear() const { delete[] data; }
};
int main() {
  Buffer b;
  int *p = b.get();
  b.clear();
  return *p;
}
