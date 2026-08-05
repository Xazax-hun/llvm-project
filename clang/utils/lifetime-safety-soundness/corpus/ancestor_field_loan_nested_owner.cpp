// DESC: a nested owner. An imprecise borrow from a [[clang::lifetimebound]]
// accessor carries the loan of the OUTER field (`d`), while the mutation names the
// INNER field (`s`). Field mutations match borrows by exact field identity, so
// nothing matched; and the conservative "an imprecise borrow into the object is
// invalidated too" fallback was suppressed because the borrow held a field loan at
// all -- on the assumption that a borrow naming a field is matched exactly. That
// holds for the mutated field or a sibling of it, but not for a field that
// CONTAINS the mutated one: mutating `s` can reallocate storage a borrow of `d`
// points into. Suppression now requires the field loan to be precise with respect
// to the mutation. Routing the same mutation through a reference (`std::string &r
// = w.d.s; r.assign(...)`) leaves MutatedField null and was reported immediately,
// which is what showed the field-precise path to be strictly weaker than the
// generic one. No parameters and no annotations beyond the sanctioned accessor.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Owner]] Doc {
  std::string s;
  std::string_view text() const [[clang::lifetimebound]] { return s; }
};

struct [[gsl::Owner]] Wrapper {
  Doc d;
};

int main() {
  Wrapper w;
  w.d.s.assign(200, 'x');
  std::string_view v = w.d.text(); // carries field `d`'s loan
  w.d.s.assign(400, 'y');          // names field `s`; reallocates
  sink = v[0];                     // heap-use-after-free
  return 0;
}
