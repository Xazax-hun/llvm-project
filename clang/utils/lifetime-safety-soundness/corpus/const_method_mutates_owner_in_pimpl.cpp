// DESC: a const member function mutates a gsl::Owner reached through a NON-owner
// pimpl behind an owning smart pointer. `Impl` is a plain class that merely
// *contains* a std::vector; a const accessor hands out a borrow into that vector
// (`get()` -> `p->v[0]`) and a const `grow()` reallocates it through the
// unique_ptr. `const` does not propagate through the smart pointer, so the
// pointee's owner is mutable even from a const method -- a const subversion.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <vector>

struct Impl {
  std::vector<int> v{1, 2, 3, 4, 5, 6, 7, 8};
};

struct Facade {
  std::unique_ptr<Impl> p = std::make_unique<Impl>();
  const int &get() const [[clang::lifetimebound]] { return p->v[0]; }
  void grow() const { p->v.push_back(99); } // reallocates p->v
};

int main() {
  Facade f;
  const int &r = f.get(); // borrow into f.p->v[0]
  f.grow();               // reallocates the buffer -> r dangles
  return r;               // use-after-free
}
