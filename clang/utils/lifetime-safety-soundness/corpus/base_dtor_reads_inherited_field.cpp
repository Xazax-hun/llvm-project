// DESC: a borrow of a destructor-body local stored into an INHERITED field. Field-escape
// facts are deliberately suppressed inside a destructor -- by the time one returns the
// object is gone, so "this borrow escapes to a field" is vacuous, and emitting it made a
// destructor's own cleanup look like it stranded a borrow. That reasoning does not hold for
// a BASE subobject: it outlives the derived destructor's BODY, its destructor runs after
// the body returns, and it can read the base's own members. The CFG marks that point with
// a base-destructor element (`~Base() (Base object destructor)`) and nothing consumed it,
// so the hazard was lost twice over: no field escape, and no use at the base's destruction
// either. A base subobject has no origin of its own, but the base's FIELDS do, and those
// are exactly what its destructor can reach. Covers multiple inheritance (one element per
// base), a virtual base, and a field of a base OF a base.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Base {
  std::string_view v;
  ~Base() { if (!v.empty()) sink = v[0]; }
};

struct [[gsl::Pointer]] Derived : Base { ~Derived(); };

Derived::~Derived() {
  std::string local = "hello world, long enough to heap allocate for sure ok";
  v = local;                    // borrow of a body-local stored into a BASE field
}

int main() { Derived d; return 0; }
