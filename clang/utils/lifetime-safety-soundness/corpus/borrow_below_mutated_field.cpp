// DESC: a borrow BELOW the mutated field. Invalidation has a field-precise path,
// used when the analysis can name the field being mutated (`o.v.clear()` names `v`),
// and a generic access-path path used otherwise. The field path asked whether the
// borrow denotes the mutated field ITSELF -- true for `o.v`, false for anything
// inside it -- and returned that answer directly, so the generic comparison was never
// reached. For `o.v[0].a` the borrow's last named field is `a`, not `v`, so the field
// test said no; the generic test would have said yes, since `o.v` is a prefix of
// `o.v.a`. Being precise about the field therefore made the analysis STRICTLY WEAKER
// than being imprecise: writing the identical code with a LOCAL vector was caught,
// and only the member spelling escaped.
//
// This is the same structural defect an earlier fix addressed for reference members;
// that fix removed one entry condition into the branch rather than the branch's
// weakness, so the hole recurred one field deeper. The fix here is for the field test
// to fall through to the generic one instead of deciding the answer. Sibling
// precision -- the reason the field path exists -- is unaffected: `o.a` and `o.b`
// still diverge.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <vector>

volatile char sink;

struct Inner {
  std::string a;
};

struct Outer {
  std::vector<Inner> v;
};

int main() {
  Outer o;
  o.v.push_back(Inner{"a string long enough to be heap allocated for sure!!"});
  std::string_view sv = o.v[0].a; // borrow of o.v.a
  o.v.clear();                    // destroys the element, freeing a's buffer
  sink = sv[0];                   // heap-use-after-free
  return 0;
}
