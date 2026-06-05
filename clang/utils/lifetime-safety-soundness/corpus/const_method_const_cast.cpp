// DESC: a const method invalidates a borrow by reallocating via const_cast
// EXPECT-ASAN: heap-use-after-free
struct [[gsl::Owner(int)]] Buffer {
  int *data;
  Buffer() : data(new int[1]) { data[0] = 42; }
  ~Buffer() { delete[] data; }
  int *get() const [[clang::lifetimebound]] { return data; }
  void grow() const {
    delete[] data;
    const_cast<Buffer *>(this)->data = new int[100];
    data[0] = 7;
  }
};
int main() {
  Buffer b;
  int *p = b.get();
  b.grow();
  return *p;
}
