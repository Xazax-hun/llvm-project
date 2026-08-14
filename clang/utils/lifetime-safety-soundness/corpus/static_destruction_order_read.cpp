// DESC: a global's destructor reading another global that has already been destroyed.
// Objects of static storage duration are destroyed in reverse order of construction,
// so the one declared FIRST is destroyed LAST and can still run code after the other
// is gone. Across translation units the order is unspecified entirely.
//
// This was invisible to the analysis for a structural reason: expiry is modeled from
// CFGLifetimeEnds, which exists only for automatic storage. A static-duration object
// therefore has no Expire fact and its access path is effectively immortal, so nothing
// distinguishes a live global from a destroyed one. Worse, the asymmetry was easy to
// hit by accident: forming a BORROW of a global trips -Wlifetime-safety-view-on-
// mutable-global, but a direct value read (`s[0]`) forms no borrow and so tripped
// nothing at all. That is the classic "logger destroyed before its clients" bug.
//
// Detection would need whole-program shutdown reachability, so the model addresses it
// by construction instead: a static-duration variable must be trivially destructible
// or carry '[[clang::destruction_order_safe]]', and that promise is verified against
// the destructor -- which may not reference another such object, nor call anything
// unchecked. Here `A` has a user-written destructor and no promise, so `A g_b;` is
// rejected outright; annotating it instead reports the read of `g_a`.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct A {
  ~A();
};

A g_a_first;                              // constructed 1st -> destroyed LAST
std::string g_str(std::string(70, 'x'));  // constructed 2nd -> destroyed FIRST

// Runs after g_str is already gone.
A::~A() { sink = g_str[0]; }

int main() { return 0; }
