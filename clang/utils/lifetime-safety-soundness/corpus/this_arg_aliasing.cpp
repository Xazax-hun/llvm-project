// DESC: a member call passes a [[clang::lifetimebound]] borrow into *this->p as
// its [[clang::noescape]] argument (`h.process(h.get())`); inside, a clean-
// looking `p = make_unique<>()` (just operator=) frees the pointee the argument
// still aliases, then writes through it. The arg-overlap check related only the
// EXPLICIT arguments to each other and ignored the implicit object (`this`), so
// the receiver-mutation-vs-argument-borrow aliasing was missed. Found by the
// 5th multi-agent bypass hunt (D2).
// EXPECT-ASAN: heap-use-after-free
#include <memory>

struct [[gsl::Owner]] Inner { int x = 7; long pad[64] = {}; };
struct [[gsl::Owner]] Holder {
  std::unique_ptr<Inner> p = std::make_unique<Inner>();
  int &get() [[clang::lifetimebound]] { return p->x; }
  void reseat() { p = std::make_unique<Inner>(); } // frees the old Inner
  void process(int &borrowed [[clang::noescape]]) {
    reseat();
    borrowed = 0xBADBAD; // write into the freed (old) Inner
  }
};

int sink;
void f() {
  Holder h;
  h.process(h.get()); // 'borrowed' aliases *h.p via the implicit this
}
int main() { f(); return sink; }
