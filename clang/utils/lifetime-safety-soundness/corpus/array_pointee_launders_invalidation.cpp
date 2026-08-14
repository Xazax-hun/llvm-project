// DESC: invalidation laundered through a reference-to-ARRAY parameter. Assumed
// invalidation asks whether a callee can reach a mutable owner through a parameter, and
// answered it from the pointee type without peeling array dimensions. An array type is
// not itself a gsl::Owner and has no CXXRecordDecl, so `std::string (&)[2]` looked like
// it reached no owner at all and the gate never opened -- while the callee can of course
// reallocate `a[0]` exactly as a `std::string &` parameter can.
//
// The sibling predicate `isMutableOwnerType` already peeled array dimensions, and so did
// the field recursion; only this one did not. The pointer form `std::string (*)[2]` and
// an array whose element is a record *containing* an owner behave the same way.
//
// The `[[clang::noescape]]` is TRUTHFUL: the parameter really does not escape, so no body
// verifier applies. Changing the parameter to `std::string &` makes it fire immediately,
// which is what localizes the defect to the missing peel.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

using Arr = std::string[2];

// Truthful annotation: `r` does not escape. It is still written through.
static void refill(Arr &r [[clang::noescape]]) {
  r[0] = std::string(200, 'y'); // frees the old buffer
}

int main() {
  std::string a[2] = {std::string(70, 'x'), std::string(70, 'z')};
  const char *v = a[0].data(); // borrow into a[0]'s heap buffer
  refill(a);                   // reallocates it through the array reference
  sink = v[0];                 // heap-use-after-free
  return 0;
}
