// DESC: the factory idiom -- an owner returned holding a borrow of a body local. A
// [[gsl::Owner]]'s members were opaque, so a borrow parked in one landed on a transient
// member-access origin and was dropped; expiry, invalidation and escape then all had nothing
// to see. Only a store through `this` or a parameter was reported, by checks that judge the
// store itself. Giving a non-public member of a TU-local owner its own origin puts the borrow
// where the ordinary machinery can reason about it, and this comes out as a plain
// return-stack-address. `d` is private exactly as owner-public-pointer advises.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile int g_sink;

class [[gsl::Owner(char)]] Box {
  const char *d = nullptr;               // private, exactly as owner-public-pointer advises
public:
  static Box make() {
    Box b;
    std::string l(64, 'x');
    b.d = l.c_str();
    return b;                            // returns an owner holding a dangling borrow
  }
  void show() const { g_sink = *d; }
};

int main() { Box b = Box::make(); b.show(); return 0; }
