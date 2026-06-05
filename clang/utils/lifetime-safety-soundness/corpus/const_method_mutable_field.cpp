// DESC: a const method invalidates a borrow by reallocating through a mutable field
// EXPECT-ASAN: heap-use-after-free
struct [[gsl::Owner(int)]] Buffer {
  mutable int *data;
  Buffer() : data(new int[1]) { data[0] = 42; }
  ~Buffer() { delete[] data; }
  int *get() const [[clang::lifetimebound]] { return data; }
  void grow() const { // const, yet reallocates via the mutable field
    delete[] data;
    data = new int[100];
    data[0] = 7;
  }
};
int main() {
  Buffer b;
  int *p = b.get();
  b.grow(); // const -> not treated as invalidating; 'mutable' is what makes it possible
  return *p;
}
