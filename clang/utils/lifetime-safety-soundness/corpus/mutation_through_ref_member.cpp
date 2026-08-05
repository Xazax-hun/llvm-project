// DESC: a "context" view aggregate holds a REFERENCE to a container; a borrow into
// an element is invalidated by mutating the container through that reference member.
// A mutating call whose receiver is a data member scopes the invalidation to that
// field, which is right for a member holding its own storage but wrong for a
// reference: a reference is an alias, so the receiver's loan names the referent, not
// the field. Field-identity matched nothing, and the early return also skipped the
// generic access-path comparison that does match, so this invalidated nothing. The
// pointer spelling of the same design always worked, since its receiver is a
// dereference rather than a MemberExpr.
//
// Note [[gsl::Pointer]] is what the model *requires* here: without it the type,
// which holds a reference, trips -Wlifetime-safety-unknown-ownership. So this is the
// shape the safe model forces on a context/view aggregate.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <vector>

volatile char sink;

struct [[gsl::Pointer]] Ctx {
  std::vector<std::string> &items;
};

int main() {
  std::vector<std::string> v;
  v.push_back("a string long enough to need its own heap buffer, not the SSO one");

  Ctx c{v};
  std::string_view sv = c.items[0]; // borrow the element's heap buffer
  c.items.clear();                  // destroys it, through the reference member
  sink = sv[0];                     // heap-use-after-free
  return 0;
}
