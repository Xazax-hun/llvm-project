// DESC: a borrow is captured into a [[gsl::Owner]] class-template
// specialization via a `[[clang::lifetime_capture_by(this)]]` method. The
// owner-capture ban (which forbids stashing a borrow into an owner's opaque
// members, since it cannot be tracked) tested the OwnerAttr directly on the
// specialization's record -- but an explicit specialization carries the
// gsl::Owner-ness only via the primary template, so `hasAttr<OwnerAttr>()` was
// false and the ban never fired. The captured borrow lands in the owner's
// private member (untracked), the owner-ref params carry a truthful
// `noescape`, and `emit()` returns void, so every other net is silent. Found by
// the 58th multi-agent bypass hunt (A). Closed by routing the ban through
// `lifetimes::isGslOwnerType`, which honors the primary-template fallback.
// EXPECT-ASAN: stack-use-after-return
int g_observed = 0;

template <class T> struct [[gsl::Owner]] Wrap { T v; };
template <> struct Wrap<int> {
private:
  const int *p_ = nullptr;

public:
  void set(const int *p [[clang::lifetime_capture_by(this)]]) { p_ = p; }
  void emit() const { g_observed = *p_; }
};

__attribute__((noinline)) void stash(Wrap<int> &w [[clang::noescape]]) {
  int local = 0xC0FFEE;
  w.set(&local); // borrow of `local` captured into w.p_
}                // `local` dies here

__attribute__((noinline)) void run(Wrap<int> &w [[clang::noescape]]) {
  w.emit(); // reads w.p_ -> dangling
}

__attribute__((noinline)) void clobber() {
  volatile int junk[64];
  for (int i = 0; i < 64; ++i)
    junk[i] = i * 7;
}

int main() {
  Wrap<int> w;
  stash(w);
  clobber();
  run(w);
  return g_observed;
}
