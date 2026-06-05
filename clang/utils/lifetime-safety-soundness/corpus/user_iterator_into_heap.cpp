// DESC: a user iterator yields a borrow into a container's heap buffer
// EXPECT-ASAN: heap-use-after-free
struct Vec {
  int *data;
  Vec() : data(new int[3]{1, 2, 3}) {}
  ~Vec() { delete[] data; }
  struct It {
    int *p;
    int &operator*() const [[clang::lifetimebound]] { return *p; }
  };
  It begin() const [[clang::lifetimebound]] { return It{data}; }
};
int main() {
  int *q;
  {
    Vec v;
    q = &*v.begin();
  }
  return *q;
}
