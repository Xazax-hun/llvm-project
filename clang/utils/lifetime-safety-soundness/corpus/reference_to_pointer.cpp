// DESC: a pointer reseated to a local through a reference-to-pointer
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *p = nullptr;
  int *&rp = p;
  {
    int x = 9;
    rp = &x; // reseats p
  }
  return *p;
}
