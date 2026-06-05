// DESC: a borrow laundered through reinterpret_cast to char* and back
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *p;
  {
    int x = 9;
    char *c = reinterpret_cast<char *>(&x);
    p = reinterpret_cast<int *>(c);
  }
  return *p;
}
