// DESC: a class-level '[[clang::destruction_order_safe]]' that never reached the override
// rule. The attribute is consulted by two questions that disagreed. "Is this TYPE safe?"
// -- which gates static-duration variables, members, bases, and `delete` -- read the
// CLASS-level attribute. "Must this OVERRIDE carry the promise?" read the attribute on
// the destructor DECLARATION only.
//
// So `struct [[clang::destruction_order_safe]] Base { virtual ~Base() = default; };` --
// the spelling the documentation shows -- made `Base` a safe type, so a verified body may
// `delete` a `Base *`, while never requiring any derived destructor to promise anything.
// `~Derived` was therefore never verified yet ran at shutdown. The `delete` check cannot
// cover this on its own: it can only judge the STATIC type, while dispatch picks the
// dynamic one, so forcing the promise onto every override is what makes it sound.
//
// Moving the attribute from the class to `~Base` made the diagnostic appear, which is what
// localized the defect to the two queries disagreeing rather than to the rule being absent.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct [[clang::destruction_order_safe]] Base {
  virtual ~Base() = default;
};

extern const std::string g_s;

// Drops the promise: never verified, but its body runs at shutdown.
struct Derived : Base {
  ~Derived() override;
};

struct [[gsl::Owner]] [[clang::destruction_order_safe]] Holder {
  ~Holder() { delete p; } // `Base` is a "safe type", so this is permitted
private:
  Base *p = new Derived;
};

Holder h;                     // dynamic init #1 -> destroyed LAST
const std::string g_s =       // #2 -> destroyed FIRST
    "a string long enough to force a heap allocation ................";

Derived::~Derived() { sink = g_s[0]; } // heap-use-after-free

int main() { return 0; }
