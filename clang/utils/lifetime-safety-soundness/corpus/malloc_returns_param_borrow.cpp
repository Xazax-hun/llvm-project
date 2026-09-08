// DESC: `__attribute__((malloc))` on a function that actually returns a borrow of its parameter.
// The attribute promises the returned pointer does not alias any object that already exists, so
// the call is modelled as a fresh allocation and the parameter-to-return propagation is never
// emitted -- the attribute switches the check off. Deleting it from the same function makes the
// dangle report immediately.
//
// Fixed by verifying the body, the way the [[clang::lifetime_immortal]] promise already is: a
// returned borrow has to be a fresh allocation, and a borrow of a parameter, the implicit object,
// a local or a global is a lie.
//
// Banning `malloc` together with [[clang::lifetimebound]] would not have been enough. The early
// return does not depend on lifetimebound being present, so the same lie written without it stays
// silent -- and there the unannotated-parameter demand suggests ADDING lifetimebound, which is the
// banned form. Verification covers both spellings.
// EXPECT-ASAN: heap-use-after-free
#include <string>
volatile char sink;
__attribute__((malloc)) const char *grab(const std::string &s [[clang::lifetimebound]]) {
  return s.c_str();
}
int main() {
  const char *p;
  { std::string s(60, 'x'); p = grab(s); }
  sink = *p;                                 // heap-use-after-free
}
