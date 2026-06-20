// DESC: a structured binding bound by reference to a heap object, which is then
// freed; reading a SCALAR binding element (`a`, type int, no origin of its own)
// did not register a use of the decomposed object, so the free's invalidation
// found no live origin and the use-after-free was missed -- silent under
// -Wlifetime-safety-soundness AND the legacy -Wdangling / -Wreturn-stack-address
// (those only model temporary lifetime-extension / stack returns). Found by the
// 64th multi-agent bypass hunt (A), escalating the round-63 structured-binding
// near-miss to a heap free that no warning caught. Closed by registering a use
// of the decomposed object when a binding element is read.
// EXPECT-ASAN: heap-use-after-free
struct P {
  int a;
  int b;
};

volatile int sink;
__attribute__((noinline)) void use(int x) { sink = x; }

__attribute__((noinline)) void target() {
  P *h = new P{0xAAAA, 0xBBBB};
  const auto &[a, b] = *h; // binding init: source valid
  delete h;                // source freed
  use(a);                  // use-after-free via scalar element 'a'
}

int main() {
  target();
  return (int)sink;
}
