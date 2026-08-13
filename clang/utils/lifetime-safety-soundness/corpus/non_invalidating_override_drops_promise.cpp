// DESC: a '[[clang::lifetime_non_invalidating]]' promise made on a virtual method and
// broken by an override that does not repeat it. The promise is consumed at the CALL
// SITE against the statically resolved callee, so dispatching through the base
// suppresses the invalidation that would otherwise be assumed. Verification, by
// contrast, is per-declaration and only checks the body carrying the attribute: the
// base's body is truthful and passes, while the override carries no promise and so is
// never checked against one. The caller's borrow is left dangling with nothing
// reported anywhere.
//
// The analysis already enforced this kind of contract across overrides for
// 'lifetime_immortal' and 'lifetimebound'; 'lifetime_non_invalidating' was simply
// missing from that list. Requiring the override to repeat the promise also routes its
// body through the existing verifier, which then reports the untruth directly.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile char sink;

class [[gsl::Owner]] Buffer {
public:
  std::vector<char> data{'a', 'b', 'c', 'd'};
  // Truthful: hands out a reference without reallocating.
  [[clang::lifetime_non_invalidating]]
  virtual char &peek(unsigned i) [[clang::lifetimebound]] {
    return data[i];
  }
  virtual ~Buffer() = default;
};

class [[gsl::Owner]] CompactingBuffer : public Buffer {
public:
  // Breaks the base's contract; nothing required it to repeat the attribute.
  char &peek(unsigned i) [[clang::lifetimebound]] override {
    data.reserve(data.capacity() * 4); // reallocates
    return data[i];
  }
};

int main() {
  CompactingBuffer cb;
  Buffer &b = cb;
  char &c = b.data[0]; // borrow into the current heap buffer
  (void)b.peek(0);     // suppressed by Buffer::peek's promise; reallocates
  sink = c;            // heap-use-after-free
  return 0;
}
