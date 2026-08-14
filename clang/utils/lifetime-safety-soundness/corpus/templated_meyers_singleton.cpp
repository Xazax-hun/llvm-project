// DESC: the templated Meyers singleton. `static T t;` inside a function template is a
// static-duration object of an arbitrary type, but the destruction-order ban ran on
// the dependent PATTERN, where `T` says nothing about the hazard, and never on the
// instantiation. So an unsafe type acquired static storage duration with no annotation
// anywhere -- the ban was skipped entirely rather than lied to. This is a very common
// idiom, which is what makes it serious.
//
// The non-template forms were all caught (a plain global, a function-local static, an
// inline variable, a variable template, a static data member of a template), so the
// gap was specifically that instantiated bodies were not visited.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

extern std::string g_str;

struct Logger {
  ~Logger() { sink = g_str[0]; } // reads a global that may already be gone
};

template <class T> T &singleton() {
  static T t; // dependent type: harmless in the pattern, unsafe once instantiated
  return t;
}

// Force the instantiation, and register its destructor BEFORE g_str is constructed
// so it runs after g_str is destroyed.
int g_force = (singleton<Logger>(), 0);

std::string g_str = "0123456789012345678901234567890123456789012345678901234567890123456789";

int main() { return 0; }
