// DESC: a standard container CONSTRUCTING a user element at shutdown. A
// '[[clang::destruction_order_safe]]' destructor may not create a local whose destructor is
// unverified, nor call an unverified function -- writing `Inner i;` directly in one is
// correctly refused. Putting the same object inside a `std::vector` silenced it.
//
// Two things combine. Every type-level rule asks whether a type's DESTRUCTOR is safe, so
// `Inner` -- trivially destructible, with a hazardous CONSTRUCTOR -- passes all of them. And
// the constructor call is made by the library: `std::vector<Inner> v(1)` runs `Inner::Inner()`
// from inside libc++, so nothing at the call site names it and the per-constructor check that
// catches a directly written `Inner i;` never sees it.
//
// That is what makes it a question about the TYPE rather than about a call, and the answer
// follows the template arguments exactly as the destruction question does. A user type's own
// constructor still gets the precise report instead, since it is visible as a
// CXXConstructExpr.
//
// The escape hatch is not a silencer: annotating the element type to clear this rule routes
// that element's constructor through the verifier, which then reports the hazard in its own
// right -- the class-level tag now promises safe construction as well as safe destruction.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <vector>

volatile char sink;

extern std::string buf;

// Trivially destructible, so no destruction rule constrains it. Its constructor is the hazard.
struct Inner {
  char c;
  Inner();
};

struct [[clang::destruction_order_safe]] Guard {
  ~Guard() {
    std::vector<Inner> v(1); // `Inner i;` here instead is correctly refused
    sink = v[0].c;
  }
};

Guard g;                                  // dyn-init #1 -> destroyed LAST
std::string buf = std::string(200, 'x');  // #2 -> destroyed FIRST

Inner::Inner() : c(buf[0]) {} // heap-use-after-free

int main() { return 0; }
