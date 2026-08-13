// DESC: a borrow through a [[clang::lifetimebound]] accessor whose access path and the
// mutated path have DIFFERENT LENGTHS. The imprecise borrow is recorded as
// `$this.*.s` and the mutation names `$this.node.v`; `.*` must be free to expand to
// `node.v`, making the two overlap. The wildcard matcher backtracked only on
// wildcards in one of the two sequences, so a `.*` in the other behaved as exactly
// one element and the comparison became length-sensitive. The paths then compared as
// non-overlapping, and since disjointness is derived from that comparison they were
// declared PROVABLY DISJOINT -- the precise unsoundness the wildcard element was
// introduced to prevent, reintroduced by its own matcher.
//
// The bug only showed when the lengths failed to line up, which is why a whole family
// of shapes (accessor on `this`, on a local, through a free function, at deeper
// nesting) slipped through while the equal-length cases were caught.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <vector>

volatile char sink;

struct Elem {
  std::string s;
};

struct Level {
  std::vector<Elem> v;
};

struct Root {
  Level node;
  Elem &first() [[clang::lifetimebound]] { return node.v[0]; }

  void bug() {
    std::string_view sv = first().s; // borrow: $this.*.s
    node.v.clear();                  // mutates: $this.node.v -- frees the element
    sink = sv[0];                    // heap-use-after-free
  }
};

int main() {
  Root r;
  r.node.v.push_back(Elem{std::string(200, 'x')});
  r.bug();
  return 0;
}
