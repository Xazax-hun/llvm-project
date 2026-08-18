// DESC: an INHERITED constructor (`using Base::Base;`). The call it makes to the base's
// constructor is the entire reason the using-declaration exists, and it is the one thing
// the constructor's initializer list does not mention -- Clang models it separately.
//
// That made the gap look covered. The initializer list is otherwise complete: a hazardous
// MEMBER of the derived class is found through it, so the descent through an implicit
// constructor appeared to work. Only the base was missed. Every neighbouring shape is
// refused -- `D(int i) : Base(i) {}` reports, `struct D : Base {}` with `D d;` reports,
// and `using Base::operator=` / `using Base::touch` were never affected -- so this is
// specific to inherited constructors.
//
// `Derived` needs no annotation of its own, which makes this a missing callee check rather
// than a mis-granted promise: one attribute-free using-declaration let an arbitrary
// unverified constructor run at shutdown.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct [[clang::destruction_order_safe]] Victim {
  std::string s;
  Victim() { s = std::string(100, 'a'); }
  [[clang::destruction_order_safe]] char peek() const { return s.data()[0]; }
};

extern Victim v;

// Not annotated, and trivially destructible, so no type-level rule constrains it.
struct Base {
  int k;
  Base(int x);
};

// The using-declaration is the whole trick.
struct Derived : Base {
  using Base::Base;
};

struct [[clang::destruction_order_safe]] Last {
  ~Last() {
    Derived d(1);
    sink = (char)d.k;
  }
};

Last last; // constructed first  -> destroyed LAST
Victim v;  // constructed second -> destroyed FIRST

// The body is right here in this TU, and was never verified against anything.
Base::Base(int x) : k(v.peek() + x) {}

int main() {
  sink = v.peek();
  return 0;
}
