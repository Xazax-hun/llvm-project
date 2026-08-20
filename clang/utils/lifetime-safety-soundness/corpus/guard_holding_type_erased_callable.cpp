// DESC: a scope guard whose destructor invokes a TYPE-ERASED callable. What the
// std::function captured is invisible behind the wrapper's interface -- here a
// by-reference capture of the caller's vector -- so calling it from the destructor
// reallocates storage the caller holds a borrow into, with nothing in the guard's
// type saying so. The destruction model asked whether the destroyed type is a
// pointer/reference or a gsl::Pointer reaching an owner, and a record holding only
// a std::function is neither; a [[gsl::Owner]] was additionally exempt on the
// grounds that destroying an owner frees what it owns, which is its job. Both the
// Owner and the Pointer spelling were silent, so the annotation was not the
// discriminator -- the erased capture was. The other callable shapes are covered
// elsewhere: a lambda held directly exposes its captures as fields, a function
// pointer is refused as an indirect call, and a capturing lambda in a plain struct
// is refused as unknown-ownership.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <functional>
#include <vector>

volatile int sink;

struct [[gsl::Owner]] Guard {
  std::function<void()> f;
  ~Guard() { f(); }
};

int main() {
  std::vector<int> vec(4);
  int *p = &vec[0];
  { Guard g{[&vec] { vec.push_back(0); }}; } // ~Guard calls f, reallocating vec
  *p = 7;                                    // heap-use-after-free
  sink = *p;
  return 0;
}
