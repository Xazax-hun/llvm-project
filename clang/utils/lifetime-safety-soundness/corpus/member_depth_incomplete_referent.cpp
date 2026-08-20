// DESC: the single-indirection rule on a data member, evaded by declaration order.
// The depth of a member is measured from its referent type, and the check ran when
// the ENCLOSING record was completed -- so a forward-declared referent contributed
// no fields yet and `Fwd &f` measured as depth 1 instead of 2. Defining Fwd before
// the holder reported the member, so the order of two declarations decided whether
// the rule applied at all. The borrow then flows through the untracked second level
// (`h.f.sv = s`) and lands on a level the origin tree does not model, so it is
// dropped and the later read dangles with nothing reported. Measuring at TU end,
// where every type in the TU is complete, makes both orders behave alike.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Fwd; // incomplete while Holder is completed

struct [[gsl::Pointer]] Holder {
  const char *keep;
  Fwd &f; // depth 2, but measured as 1
};

struct [[gsl::Pointer]] Fwd {
  std::string_view sv;
};

int main() {
  std::string keep = "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk";
  Fwd obj{keep};
  Holder h{keep.data(), obj};
  {
    std::string s = "ssssssssssssssssssssssssssssssssssss";
    h.f.sv = s; // borrow stored through the untracked second level
  }             // s dies
  sink = obj.sv[0]; // heap-use-after-free
  return 0;
}
