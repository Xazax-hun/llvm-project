// DESC: arbitrary code laundered into shutdown by an IMPLICIT constructor. A
// '[[clang::destruction_order_safe]]' destructor may not create a local whose destructor is
// unverified, and may not call an unverified function -- writing `Inner i;` directly in the
// destructor below is correctly refused. Wrapping `Inner` in an attribute-free aggregate
// silences it completely.
//
// Two things combine. Every type-level rule asks whether a type's DESTRUCTOR is safe, so
// `Inner` -- trivially destructible, with a hazardous CONSTRUCTOR -- passes all of them. And
// the body checker treated an implicit constructor as inert apart from its default member
// initializers, so the constructors it invokes for each base and member were never visited.
// Those calls appear nowhere in the verified body.
//
// A constructor that is implicit or defaulted cannot carry the promise -- there is no body to
// make one about, and the attribute on a `= default` says nothing about what the compiler
// generates -- so the fix is to descend into what it actually runs rather than to demand an
// annotation. The same shape reaches through an implicit BASE initializer, through a template
// wrapper, and through the implicit initializers of a verified constructor.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

extern std::string buf;

// Trivially destructible, so no type rule constrains it. Its constructor is the hazard.
struct Inner {
  char c;
  Inner();
};

// No annotation, no user-declared constructor: the implicit default constructor is what
// launders Inner::Inner() into shutdown.
struct Outer {
  Inner i;
};

struct [[clang::destruction_order_safe]] Guard {
  ~Guard() {
    Outer o; // `Inner i;` here instead is correctly refused
    sink = o.i.c;
  }
};

Guard g;                                  // dyn-init #1 -> destroyed LAST
std::string buf = std::string(200, 'x');  // #2 -> destroyed FIRST

Inner::Inner() : c(buf[0]) {} // heap-use-after-free

int main() { return 0; }
