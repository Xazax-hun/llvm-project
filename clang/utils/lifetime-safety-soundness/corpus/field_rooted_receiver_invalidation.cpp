// DESC: a borrow into a container element reached through a FIELD, then mutated
// via a non-const method whose receiver's static type does not reveal an owner
// (a base reference `Base& b = w.d`). The assumed-invalidation gate confirms the
// receiver denotes a mutable owner from the loans it carries, but the loan roots
// at the ENCLOSING object `w` (Wrapper), whose type is not is-a the receiver's
// static type (Base), so the is-a check failed and the invalidation was dropped.
// Now the gate also recognizes an owner-containing by-value subobject of the
// loan's record that is-a the receiver type. A plain LOCAL receiver was already
// caught; only the field-reached / enclosing-object-rooted form slipped.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct Base {
  virtual ~Base() = default;
  void grow();
};
struct Derived : Base {
  std::vector<int> data{1, 2, 3};
};
void Base::grow() { static_cast<Derived *>(this)->data.reserve(100000); }

struct Wrapper {
  Derived d;
};

volatile int sink;

int main() {
  Wrapper w;
  int *p = &w.d.data[0]; // borrow into the vector buffer (field-reached)
  Base &b = w.d;         // receiver static type Base -- no owner field
  b.grow();              // reallocates w.d.data through the base reference
  sink = *p;             // heap-use-after-free
  return 0;
}
