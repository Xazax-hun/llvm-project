// DESC: a borrow of a mutable global taken inside a callee it was passed to by reference.
// The borrow roots at a PARAMETER placeholder while the mutation roots at the global, so the
// two never intersect and the precise invalidation check cannot connect them. The
// conservative assumed-invalidation backstop does not fire either: it is suppressed when the
// mutated receiver is a global (the `global.method()` exemption). The only remaining net was
// -Wlifetime-safety-view-on-mutable-global, which fires where a borrow of a global is
// CREATED -- and binding the global to a reference parameter was not treated as creating one.
//
// Binding a LOCAL reference (`std::string &r = g;`) was always caught, because every use of
// `r` is a DeclRefExpr whose declaration has reference type, and both guards involved test
// the declaration's type. At a call there is no such declaration: the argument is bare `g`,
// textually identical to reading it for a copy, and the reference exists only in the
// callee's signature. So the borrow was dropped for a non-const parameter and discarded as a
// copy for a const one -- while `f(&g)` was flagged all along, since a pointer expression is
// something neither guard looks at.
//
// The mutation-target spelling is irrelevant: a plain global, a static data member, a
// thread_local, a global container method and a global array element all behaved the same
// way. `this` was never affected -- a $this-rooted borrow does intersect a global-rooted
// invalidation.
//
// The `[[clang::noescape]]` is TRUTHFUL: `s` really does not escape.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

std::string g = "0123456789012345678901234567890123456789012345678901234567890123456789";

// Truthful annotation: `s` does not escape. The borrow into it still dangles.
static void reads_then_mutates(std::string &s [[clang::noescape]]) {
  const char *p = s.data(); // borrow into g's heap buffer, rooted at the parameter
  g = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  sink = p[0]; // heap-use-after-free
}

int main() {
  reads_then_mutates(g);
  return 0;
}
