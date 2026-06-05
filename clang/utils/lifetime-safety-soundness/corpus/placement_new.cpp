// DESC: placement-new into a local buffer; the returned pointer dangles
// EXPECT-ASAN: stack-use-after-return
#include <new>
__attribute__((noinline)) int *make() {
  alignas(int) char buf[sizeof(int)];
  return new (buf) int(42); // points into the local 'buf'
}
int main() { return *make(); }
