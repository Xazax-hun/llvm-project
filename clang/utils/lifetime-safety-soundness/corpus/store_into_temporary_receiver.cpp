// DESC: a borrow stored into a member of a TEMPORARY whose destructor reads it. Two
// gaps had to line up. (1) A temporary is storage like any other and has an origin
// node, but getOriginForAccessPath did not resolve one, so the store routed nowhere;
// that was thought harmless because a non-extended temporary dies at the end of the
// full expression and nothing could read the borrow afterwards -- except its own
// DESTRUCTOR, which runs at that very cleanup. (2) Temporaries are destroyed in
// reverse order of CONSTRUCTION, and a destructor only sees a borrow as dangling once
// the borrowed-from temporary's expiry has been emitted; the CFG's list is in
// syntactic pre-order, so whether the temporary destroyed LAST came last was
// accidental.
//
// The spelling matters and is not a matter of unspecified order: `a.operator=(b)`
// sequences the object expression first, so the receiver is constructed first and
// destroyed LAST, and its destructor reads freed storage. Written `a = b` the right
// operand is sequenced first, the receiver is destroyed FIRST, and there is no bug --
// so routing the store without fixing the destruction order turned that safe spelling
// into a false positive.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink;

struct [[gsl::Owner(char)]] W {
  // Runs after the string temporary is gone, and reads the borrow.
  ~W() { g_sink = v[0]; }
  W() = default;
  friend int main();

private:
  std::string_view v;
};

int main() {
  W().v.operator=(std::string("a heap string long enough to never be SSO here"));
  return 0;
}
