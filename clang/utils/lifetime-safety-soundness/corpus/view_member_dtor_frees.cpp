// DESC: a [[gsl::Pointer]] view has a MEMBER subobject (through a plain wrapper)
// whose destructor frees a borrowed pointer. Destroying the view runs the
// member's destructor chain (~Wrap -> ~Freer -> delete[]), freeing storage the
// view's own borrow (and a caller alias) points into. The round-72 view-subobject
// check (warn_lifetime_safety_view_base_may_deallocate) iterated only the view's
// BASES, not its direct members, so a freeing member slipped. Found by the
// multi-agent bypass hunt (regression-probe of the base-only check). Fixed by
// also walking the view's members through destructorMayDeallocate.
// EXPECT-ASAN: heap-use-after-free
struct Freer {
  int *p;
  ~Freer() { delete[] p; }
};

struct Wrap {
  Freer f;
};

struct [[gsl::Pointer(int)]] V {
  Wrap w;
  int *v;
};

volatile int sink;
int main() {
  int *h = new int[32];
  for (int i = 0; i < 32; i++)
    h[i] = i * 7;
  int *alias;
  {
    V view{{{h}}, h}; // w.f.p = h and v = h via aggregate init
    alias = view.v;
    sink = view.v[3];
  } // ~V -> ~Wrap -> ~Freer -> delete[] h ; alias now dangles
  sink = alias[3]; // use-after-free
  return sink;
}
