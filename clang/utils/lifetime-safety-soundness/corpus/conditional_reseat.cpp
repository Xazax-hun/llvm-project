// DESC: a pointer conditionally reseated to a local that then dies
// EXPECT-ASAN: stack-use-after-scope
int main(int argc, char **) {
  int outer = 1;
  int *p = &outer;
  if (argc > 0) {
    int inner = 2;
    p = &inner;
  }
  return *p;
}
