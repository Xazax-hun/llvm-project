// DESC: a view member in one base bound to an owner member in a SIBLING base. An
// upcast denotes the same object as its operand, so the object's loan must carry
// through it -- but when the derived and base origin shapes differ (here the owner
// base holds no origins at all) the flow was skipped SILENTLY, where the downcast
// already handled the same mismatch by carrying the outer loan and seeding the rest
// as unknown.
//
// So a member of a base subobject reached through the implicit `this` upcast started
// from an EMPTY origin and carried no loan, and the self-referential store went
// unrecorded. Bases are destroyed in reverse declaration order, so the owner base is
// freed before the view base's destructor reads the borrow into it. The identical
// store with the owner as a DIRECT member was reported all along.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink;

struct [[gsl::Pointer(char)]] ViewBase {
  std::string_view v;
  // Runs AFTER ~OwnerBase, which frees what `v` points into.
  ~ViewBase() { g_sink = v[0]; }
};

// No origins of its own: exactly the shape mismatch that dropped the flow.
struct OwnerBase {
  std::string owned{"a heap string long enough to never be SSO at all here"};
};

struct [[gsl::Pointer(char)]] Guard : ViewBase, OwnerBase {
  Guard() { v = owned; }
};

int main() {
  Guard g;
  return 0;
}
