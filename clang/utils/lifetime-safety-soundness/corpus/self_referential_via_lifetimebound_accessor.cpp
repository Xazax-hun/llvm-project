// DESC: a self-referential store laundered through a [[clang::lifetimebound]] accessor. A
// view member bound to a sibling member makes the object self-referential: mutating or moving
// the object invalidates the view, which cannot be tracked once the object is passed
// elsewhere. `title = text;` is correctly refused. Routing the identical store through an
// accessor was accepted.
//
// -Wlifetime-safety-self-referential matched when the stored loan's access path roots at a
// NAMED sibling member (`$this.text`). A lifetimebound accessor re-roots its result at the
// object and projects `.*` -- "a member of $this, identity unknown" -- so "member bound to
// sibling member" no longer matched, and a $this-rooted loan stored into a field of $this
// looked benign. The store was then never recorded at all, which is why `appendBody()` in a
// DIFFERENT method has no live borrow to invalidate: the reallocation is real, but nothing
// knows a view points into the old buffer.
//
// The two annotations that open this are exactly the two the analysis demands. Drop
// [[gsl::Owner]] and it reports unknown-ownership; drop [[clang::lifetimebound]] and it
// reports unannotated-indirection plus a suggestion to add lifetimebound. So this is the shape
// an adopter arrives at by following the analysis's own advice.
//
// `.*` counts as a member borrow only here. isFieldBorrowOf is shared with the invalidation
// and argument-overlap checks, where widening a precise question to "some member" turns
// disjoint members into apparent aliases -- it cost five false positives on the demo app.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

class [[gsl::Owner]] Doc {
  std::string text;
  std::string_view title; // a cached view of our own buffer

  std::string_view raw() const [[clang::lifetimebound]] { return text; }

public:
  void load() { text.assign(70, 'a'); }
  void parse() { title = raw(); }               // `title = text;` here is refused
  void appendBody() { text.append(4000, 'z'); } // reallocates; title now dangles
  char titleFirst() const { return title.empty() ? 'e' : title[0]; }
};

int main() {
  Doc d;
  d.load();
  d.parse();
  d.appendBody();
  volatile char c = d.titleFirst(); // heap-use-after-free
  (void)c;
  return 0;
}
