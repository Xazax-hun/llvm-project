// DESC: a std::string owner field of a derived class is reallocated by an
// overridden VIRTUAL method dispatched through a Base& whose static type
// declares no owner field. The assumed-invalidation gate tested the static
// receiver type (recovered with IgnoreImpCasts after round 51), but a virtual
// call through a base reference can only recover the BASE static type, not the
// dynamic Derived; recordHasGslOwnerField(Base) was false and the invalidation
// was skipped, leaving the string_view tracked-valid across the realloc: a
// silent heap-use-after-free. A non-const virtual call on a polymorphic
// receiver is now conservatively assumed to mutate an owner.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <cstdio>
struct Base {
  virtual const char *view() const [[clang::lifetimebound]] = 0;
  virtual void grow() = 0;
  virtual ~Base() = default;
};
struct Derived : Base {
  std::string buf = "a long heap string exceeding sso limits..........";
  const char *view() const [[clang::lifetimebound]] override { return buf.data(); }
  void grow() override { buf.append(2000, 'z'); } // reallocates buf
};
__attribute__((noinline)) int bug() {
  Derived d;
  Base &b = d;                  // dispatch through Base&
  std::string_view v(b.view());
  b.grow();                     // virtual: static receiver Base (no owner field)
  return v.empty() ? 0 : v[0];  // reads freed heap
}
int main() { printf("%d\n", bug()); return 0; }
