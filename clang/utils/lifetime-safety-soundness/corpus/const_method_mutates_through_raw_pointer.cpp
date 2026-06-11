// DESC: a const member function mutates an owner (std::vector) through the
// pointee of a RAW pointer data member. `const` does not propagate through a raw
// pointer any more than through a smart pointer, so the pointee is mutable even
// though `this` is const; the analysis would otherwise trust that a const member
// function does not invalidate borrows into the object. The owning object is a
// [[gsl::Pointer]] whose raw member is populated via a lifetime_capture_by(this)
// constructor (so no other soundness warning fires). Found by the 2nd
// multi-agent bypass hunt.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct [[gsl::Pointer]] Manager {
  std::vector<int> *ptr;
  Manager(std::vector<int> *r [[clang::lifetime_capture_by(this)]]) : ptr(r) {}
  // const, yet reallocates *ptr through the raw pointer member.
  void grow() const { ptr->push_back(42); }
};

int main() {
  std::vector<int> v;
  v.push_back(1);
  Manager m(&v);
  int *p = &v[0]; // borrows v's buffer
  m.grow();       // reallocates v -> p dangles
  return *p;      // use-after-invalidation
}
