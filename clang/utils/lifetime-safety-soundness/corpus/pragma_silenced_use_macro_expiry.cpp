// DESC: a `#pragma clang diagnostic ignored` around a use that never runs hid the
// unsilenced use after it, because the borrow's scope ended inside a macro
// expansion: a macro location compares greater than every file location, so
// every use looked "before" the expiry and the decision fell back to the one
// named (silenced) use.
// EXPECT-ASAN: stack-use-after-scope
volatile int sink;
#define BORROW_TEMP(ptr) { int x = 7; ptr = &x; }
int main(int argc, char **) {
  int *p = nullptr;
  BORROW_TEMP(p)
  if (argc > 99) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlifetime-safety-use-after-scope"
    sink += *p;
#pragma clang diagnostic pop
  }
  sink += *p;
  return sink == 12345;
}
