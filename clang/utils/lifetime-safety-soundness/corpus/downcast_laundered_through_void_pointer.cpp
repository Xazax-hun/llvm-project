// DESC: a base-to-derived conversion laundered through `void *`. Bases are destroyed AFTER
// the derived part, so by the time `~Base` runs every heap buffer a derived member owned has
// been freed -- reading derived state from a base destructor is a use-after-free, and the
// analysis cannot see it because it models `this` as a live complete object.
//
// Base-to-derived conversions are refused outright for that reason, but the test compares the
// source and target types, and splitting the conversion in two through `void *` defeats it:
// `Base * -> void *` has no record on the target side and `void * -> Derived *` none on the
// source side, so neither half looks like a downcast. Nothing else caught it either --
// -Wlifetime-safety-type-punning covered `reinterpret_cast` but this is a `static_cast` (a
// C-style cast works identically), and no `lost-loan` fired because the loan flows through the
// `void *` intact, so the borrow never looks lost.
//
// The fix is that recovering a typed pointer OUT of a `void *` launders provenance exactly as
// a `reinterpret_cast` does -- `void *` is opaque, so the conversion can name any type -- and
// is refused on the same grounds. Casting *to* `void *` stays allowed: that is the
// opaque-userdata idiom, and what a callee may do with such a parameter is handled
// conservatively at the call site instead.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct Derived;
struct Base {
  virtual ~Base();
};
struct Derived : Base {
  std::string s = std::string(200, 'x');
};

// By the time this runs, ~Derived has already freed `s`'s buffer.
Base::~Base() {
  void *p = this; // Base * -> void *: no record on the target side
  sink = static_cast<Derived *>(p)->s[0]; // void * -> Derived *: none on the source side
}

int main() {
  Derived d;
  sink = d.s[1];
  return 0;
}
