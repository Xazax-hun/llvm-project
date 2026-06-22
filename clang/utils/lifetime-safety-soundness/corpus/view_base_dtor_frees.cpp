// DESC: a [[gsl::Pointer]] view's BASE subobject destructor frees a borrowed
// member, with view-ness declared on the DERIVED class. Destroying the view runs
// the base's `~FreerBase`, freeing the caller-owned borrow slipped into the
// inherited member by aggregate base-init -- a dangling alias. The round-71 view
// naked-delete fix keyed on the destructor's syntactic parent (FreerBase, a plain
// type, still dtor-exempt), so the freeing base destructor was not flagged. Found
// by the multi-agent bypass hunt. Fixed by a cross-TU-sound declaration-side rule
// on the view: every base/member subobject destructor that runs when the view is
// destroyed must provably not deallocate (trivial, a gsl::Owner/gsl::Pointer, or
// a visible non-deallocating body), checked recursively.
// EXPECT-ASAN: heap-use-after-free
struct Big {
  int a[64];
  int v;
};

struct FreerBase {
  Big *p;
  ~FreerBase() { delete p; } // a view owns nothing -- this must not deallocate
};

struct [[gsl::Pointer(Big)]] View : FreerBase {};

volatile int sink;
int main() {
  Big *x = new Big();
  x->v = 7;
  { View v{{x}}; } // aggregate base-init sets the inherited p = x; ~FreerBase frees x
  sink = x->v;     // use-after-free
  return sink;
}
