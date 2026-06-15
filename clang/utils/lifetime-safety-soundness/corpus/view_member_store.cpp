// DESC: a store into a raw borrow-holding member of a [[gsl::Pointer]] view
// object (`v.p = local`) was silently dropped -- a gsl::Pointer is a leaf in the
// origin tree (members not tracked per field), so the store landed on a
// transient member-access origin disconnected from `v`, while `v` kept the
// borrow it inherited from `V v = src;` (masking the loss). The store now merges
// the stored value's loans into the view's own origin (without killing), so a
// use of the view after the borrow's source expires is caught.
// FLAGS: -Wno-unused
// EXPECT-ASAN: stack-use-after-scope
struct [[gsl::Pointer]] V {
  const char *p;
  unsigned n;
};

volatile int sink;

unsigned run(V src [[clang::noescape]]) {
  V v = src; // v inherits src's (valid) borrow -- masks the dropped store
  {
    char local[64];
    for (int i = 0; i < 63; ++i)
      local[i] = 'Q';
    local[63] = 0;
    v.p = local; // store a borrow into the view member
    v.n = 63;
  } // local dies -> v.p dangles
  unsigned acc = 0;
  for (unsigned i = 0; i < v.n; ++i)
    acc += (unsigned char)v.p[i]; // use-after-scope
  return acc;
}

int main() {
  char buf[64] = {0};
  V s{buf, 0};
  sink = (int)run(s);
  return 0;
}
