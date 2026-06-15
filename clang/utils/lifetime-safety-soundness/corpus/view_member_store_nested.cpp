// DESC: a NESTED store into a view member reached through another view member
// (`v.inner.p = local`) was silently dropped -- both the outer object and its
// `inner` member are leaves in the origin tree, so the immediate base
// (`v.inner`) is a transient member-access origin disconnected from `v`, while
// `v` kept the borrow it inherited from `Outer v = src;` (masking the loss).
// The merge now climbs the origin-tree parent chain to the OUTERMOST object
// `v`, so a use of `v.inner.p` after the borrow's source expires is caught.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-scope
struct [[gsl::Pointer]] V {
  const char *p;
  unsigned n;
};

struct [[gsl::Pointer]] Outer {
  V inner;
  const char *q;
};

volatile int sink;

unsigned run(Outer src [[clang::noescape]]) {
  Outer v = src; // v inherits src's (valid) borrow -- masks the dropped store
  {
    char local[64];
    for (int i = 0; i < 63; ++i)
      local[i] = 'Q';
    local[63] = 0;
    v.inner.p = local; // nested store of a borrow into the view member
    v.inner.n = 63;
  } // local dies -> v.inner.p dangles
  unsigned acc = 0;
  for (unsigned i = 0; i < v.inner.n; ++i)
    acc += (unsigned char)v.inner.p[i]; // use-after-scope
  return acc;
}

int main() {
  char buf[64] = {0};
  Outer s{V{buf, 0}, nullptr};
  sink = (int)run(s);
  return 0;
}
