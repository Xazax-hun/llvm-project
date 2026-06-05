// DESC: returning a reference to a by-value parameter
// EXPECT-ASAN: stack-use-after-return
__attribute__((noinline)) int &f(int x) { return x; }
int main() { return f(5); }
