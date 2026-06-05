// DESC: a GNU statement-expression yields the address of a local that then dies
// EXPECT-ASAN: stack-use-after-scope
int main() {
  int *p = ({
    int x = 7;
    &x;
  });
  return *p;
}
