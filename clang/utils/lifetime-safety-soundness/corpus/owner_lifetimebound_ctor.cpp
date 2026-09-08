// DESC: a [[gsl::Owner]] constructor takes its argument [[clang::lifetimebound]] and then FREES
// it. The caller keeps its own pointer, the owner's destructor deletes the allocation, and the
// caller reads freed memory -- with nothing reported.
//
// On a constructor the annotation describes the CONSTRUCTED OBJECT, so what it declares here is
// a borrow resting in an owner. That is the same statement `lifetime_capture_by` naming an owner
// is already refused for ("an owner is meant to own its contents"), just spelled differently.
//
// It is also worse than saying nothing: the annotation SILENCES the demand to annotate the
// parameter. The identical constructor with no annotation is refused, and with [[noescape]] it is
// reported as escaping -- only the lifetimebound spelling was silent. And the suggestion
// machinery actively recommended it, so an author following the analysis's own advice would have
// introduced this.
//
// Now refused, and the suggestion no longer offers it for an owner. There is deliberately no
// annotation that makes this constructor safe: an owner that takes a borrow it may then free has
// to change its type, not its annotations.
// EXPECT-ASAN: heap-use-after-free
volatile int sink;
class [[gsl::Owner(int)]] Box {
  int *p;
public:
  explicit Box(int *q [[clang::lifetimebound]]) : p(q) {}
  ~Box() { delete p; }
};
int main() {
  int *raw = new int(7);
  { Box b(raw); }        // ~Box frees it
  sink = *raw;           // heap-use-after-free
}
