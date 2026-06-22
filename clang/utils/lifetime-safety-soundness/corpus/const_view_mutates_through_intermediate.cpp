// DESC: a [[gsl::Pointer]] view's CONST method mutates a [[gsl::Owner]] reached
// THROUGH A NON-OWNER INTERMEDIATE pointee (`this->p->v.push_back(...)`, where
// `p` is a `Holder*` and `Holder` merely contains a std::vector owner). `const`
// does not propagate through a raw pointer, so the owner is mutable; a caller
// that co-holds a borrow into that owner has it silently invalidated by the
// reallocation. The const-method invalidation path is gated on a non-const
// method, and the const-subversion handler previously only fired when the
// pointer member pointed DIRECTLY at an owner -- a non-owner intermediate
// (Holder) slipped through. Found by the multi-agent bypass hunt. Fixed by
// matching a mutable owner reached from `this` through any pointer/reference
// indirection (recordContainsMutableOwner), mirroring the non-const path.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct Holder {
  std::vector<int> v;
};

struct [[gsl::Pointer]] View {
  Holder *p;
  void grow() const { p->v.push_back(42); } // mutates the owner through `p`
};

int main() {
  Holder h{{1, 2, 3, 4, 5, 6, 7, 8}};
  int &ref = h.v[0]; // borrow into h.v's heap buffer
  View vw{&h};       // aggregate-init view holding &h
  vw.grow();         // const method reallocates h.v -> ref dangles
  return ref;        // use-after-free
}
