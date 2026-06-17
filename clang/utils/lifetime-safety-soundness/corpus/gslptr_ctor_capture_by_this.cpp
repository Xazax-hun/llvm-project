// DESC: a [[gsl::Pointer]] view with a const& reference member captured via a
// '[[clang::lifetime_capture_by(this)]]' CONSTRUCTOR parameter, plus a sibling
// pointer member set from a long-lived (const global) owner. capture_by(this)
// on a constructor is unmodeled (the captured borrow lands on a member origin,
// not the gsl::Pointer leaf origin), so the reference member's borrow of the
// local is dropped; the sibling pointer member's valid global loan then MASKS
// the would-be lost-loan, so make() was silent. The construct is now rejected
// at the constructor declaration (-Wlifetime-safety-ctor-capture): the intent
// "the constructed object may refer to this parameter" is expressed by
// [[clang::lifetimebound]], which the analysis tracks precisely.
// EXPECT-ASAN: stack-use-after-return
#include <cstdio>
struct [[gsl::Owner(int)]] Box {
  int v;
  const int *data() const [[clang::lifetimebound]] { return &v; }
};
struct [[gsl::Pointer(int)]] View {
  const int *p;
  const int &extra;
  View(const Box &b [[clang::lifetimebound]],
       const int &e [[clang::lifetime_capture_by(this)]])
      : p(b.data()), extra(e) {}
  int readExtra() const { return extra; }
};
static const Box g_box{100};

__attribute__((noinline)) View make() {
  int local = 42;
  View v(g_box, local); // extra <- &local (dropped); p <- &g_box (valid, masks)
  return v;             // v.extra dangles to the dead `local`
}
__attribute__((noinline)) void clobber() {
  volatile int junk[64];
  for (int i = 0; i < 64; i++)
    junk[i] = i;
}
int main() {
  View v = make();
  clobber();
  printf("%d\n", v.readExtra()); // reads make()'s destroyed `local`
  return 0;
}
