// DESC: a borrow taken through an explicit base-to-derived downcast on the
// member-access base -- `static_cast<Derived*>(base)->owner_field` (or the
// reference form `static_cast<Derived&>(base_ref).owner_field`) -- must keep the
// object's loan, so destroying the object through the plain (un-cast) base
// pointer is still connected. The downcast (CK_BaseToDerived / CK_Dynamic) was
// unhandled in the cast switch and dropped the origin flow, so the borrow rooted
// at nothing and neither use-after-free nor naked-delete fired. Now the object
// loan flows through the downcast. Sibling: dynamic_cast has the same shape.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct Base {
  virtual ~Base() = default;
};
struct Derived : Base {
  std::string s = "a long heap-backed string exceeding the SSO threshold, ok!!";
};

int main() {
  Base *b = new Derived();
  std::string_view v = static_cast<Derived *>(b)->s; // borrow via downcast
  delete b;                                          // free via plain base ptr
  return static_cast<int>(v.size() ? v[0] : 0);      // use of dangling view
}
