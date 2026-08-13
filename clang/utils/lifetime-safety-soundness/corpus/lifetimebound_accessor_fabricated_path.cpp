// DESC: a borrow taken through a [[clang::lifetimebound]] accessor, then invalidated
// by mutating the field it actually points into. The accessor's result carries the
// whole object's loan -- the annotation promises only "somewhere inside it", not
// which subobject -- and a member access on that result appended a concrete field to
// it. That fabricated an access path naming storage which need not exist: `w.get().s`
// became `w.s`, while the real borrow is `w.d.s`. Both are rooted at `w` and neither
// is a prefix of the other, so the field-sensitive disjointness test concluded they
// could not alias and skipped the report.
//
// This was a REGRESSION introduced by the field-sensitivity port: before it, the
// accessor's loan stayed as plain `w` and matched the mutation by containment. It is
// the hazard that port's own commit message warned about -- a wrong disjointness
// claim is unsound, because the analysis asserts a falsehood rather than merely
// losing precision.
//
// The fix records the imprecision as an Interior (`.*`) element, which stands for
// zero, one or many member accesses, so `w.*.s` may-matches the real `w.d.s` instead
// of contradicting it. Disjoint concrete siblings (`w.a` vs `w.b`) still diverge.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Doc {
  std::string s = "a string long enough to be heap allocated for sure!!";
};

struct Wrap {
  Doc d;
  Doc &get() [[clang::lifetimebound]] { return d; }
};

int main() {
  Wrap w;
  std::string_view v = w.get().s; // borrow of w.d.s, imprecisely known
  w.d.s.append(400, 'x');         // reallocates it
  sink = v[0];                    // heap-use-after-free
  return 0;
}
