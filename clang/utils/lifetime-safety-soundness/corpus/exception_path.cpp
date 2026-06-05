// DESC: a borrow dangles only along an exception path (unwinding not modeled)
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *p = nullptr;
  try {
    int x = 5;
    p = &x;
    throw 1; // unwinds; x is destroyed
  } catch (...) {
  }
  return *p; // x is dead here
}
