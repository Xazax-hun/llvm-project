// DESC: a function template returns the address of a local (bug at instantiation)
// EXPECT-ASAN: stack-use-after-return
template <class T> __attribute__((noinline)) T *mk() {
  T x{};
  return &x;
}
int main() { return *mk<int>(); }
