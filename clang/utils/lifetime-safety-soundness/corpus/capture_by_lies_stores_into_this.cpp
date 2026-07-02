// DESC: a [[clang::lifetime_capture_by(X)]] parameter that lies -- it names a
// by-value parameter 'decoy' as the capturer, but the body stores the borrow
// into a field of *this. The annotation suppresses the unannotated-indirection
// backstop and diverts the modeled capture to the dead-at-return decoy, so the
// real capture into the object went unchecked. Body verification of
// lifetime_capture_by must reject it (the borrow escapes into `this`, which the
// annotation's named capturer does not describe).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct [[gsl::Owner]] Box {
  Box() {}
  // LIE: capturer is *this, not decoy.
  void put(Box decoy, std::string_view s [[clang::lifetime_capture_by(decoy)]]) {
    p = s.data();
  }
  char read() const { return *p; }

private:
  const char *p = nullptr;
};

volatile char observed;

int main() {
  Box self;
  {
    std::string tmp(50, 'x'); // heap-allocated (beyond SSO)
    Box decoy;
    self.put(decoy, tmp); // tmp freed at end of block; self.p dangles
  }
  observed = self.read(); // heap-use-after-free
  return 0;
}
