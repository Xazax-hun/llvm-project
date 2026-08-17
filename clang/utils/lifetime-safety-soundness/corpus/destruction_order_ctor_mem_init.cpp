// DESC: a '[[clang::destruction_order_safe]]' constructor whose member-initializer list reads
// an already-destroyed static. The body verifier walked FunctionDecl::getBody(), which for a
// constructor is only the compound statement -- the mem-initializer list hangs off the
// CXXConstructorDecl, not off the body. So a constructor marked exactly as the documentation
// prescribes, with a verified body of `{}`, could initialize a member from a global that
// shutdown had already destroyed.
//
// The asymmetry is what gives it away: an in-class default member initializer WAS seen (the
// checker traverses those from the implicit constructor), and mem-init lists ARE seen by the
// noescape and lifetimebound verifiers. Only this walk missed them.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct [[clang::destruction_order_safe]] Client {
  ~Client();
};

Client c;                                 // dyn-init #1 -> destroyed LAST
std::string g_str = std::string(70, 'x'); // #2 -> destroyed FIRST

// Marked, so ~Client is allowed to call it. Its BODY is empty and verifies clean; the
// reference to the destroyed global hides in the initializer list.
struct P {
  char v;
  [[clang::destruction_order_safe]] P() : v(g_str[0]) {}
};

Client::~Client() {
  P p;
  sink = p.v; // heap-use-after-free happened in P::P
}

int main() { return 0; }
