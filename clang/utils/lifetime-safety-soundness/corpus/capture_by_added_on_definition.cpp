// DESC: an out-of-line DEFINITION adding [[clang::lifetime_capture_by]] that the
// in-class declaration does not have. A call is checked against the declaration in
// scope, so the added contract is invisible to every call that precedes the
// definition: the declaration promises no capture, the caller passes a borrow that
// dies right after the call, and the definition parks it in the object.
//
// Neither end reports -- the call is checked against the declaration, and the
// definition's body does exactly what its own annotation permits. The same hole as
// adding the attribute in a virtual override, without needing a virtual; C++ has the
// same rule for carries_dependency, that the first declaration must specify it.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char g_sink;

static const char kImmortal[] = "immortal storage long enough to never be SSO";

struct [[gsl::Pointer(char)]] D {
  const char *held;
  D(const char *a [[clang::lifetimebound]]) : held(a) {}
  // Declares no capture: callers may pass a borrow that dies right after the call.
  const char *pick(const std::string &a [[clang::lifetimebound]]);
};

int main() {
  D d{kImmortal};
  {
    std::string x("a heap string long enough to never be SSO at all here");
    (void)d.pick(x); // checked against the in-class declaration
  }
  g_sink = *d.held;
  return 0;
}

const char *D::pick(const std::string &a [[clang::lifetimebound]]
                    [[clang::lifetime_capture_by(this)]]) {
  held = a.c_str();
  return held;
}
