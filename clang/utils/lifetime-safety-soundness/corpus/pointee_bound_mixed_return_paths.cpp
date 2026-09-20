// DESC: '[[clang::lifetimebound(pointee)]]' whose body returns the object's OWN
// storage on one path and its referent on another. The body verification treated
// "verified" as a MAY property -- one referent-bound return path put the method in
// the verified set and nothing could take it out -- so the m_own path was licensed
// too, while the call site drops the object binding for every path because the
// annotation says the result refers to the referent, not the object. A borrow of
// the object's own buffer therefore escapes the object's scope with nothing
// reported; plain '[[clang::lifetimebound]]' catches it and no annotation at all
// draws a sound refusal, so the hole is specific to the pointee flavour. Found by
// the multi-agent bypass hunt on the new annotation. Closed by tracking refutation
// separately from verification, so an object-rooted return path refutes the
// promise no matter which path the escape walk reaches first.
// EXPECT-ASAN: stack-use-after-scope
#if __has_warning("-Wlifetime-safety-soundness")
#define LB_POINTEE [[clang::lifetimebound(pointee)]]
#else
#define LB_POINTEE
#endif

volatile char sink;
char g_backing[8] = {'g', 0};

struct [[gsl::Pointer(char)]] Hybrid {
  const char *m_ref;
  char m_own[8];
  Hybrid(const char *r) : m_ref(r) {
    m_own[0] = 'z';
    m_own[1] = 0;
  }
  // Honest on the m_ref path, a lie on the m_own path.
  const char *data(bool own) const LB_POINTEE {
    return own ? m_own : m_ref;
  }
};

int main() {
  const char *p;
  {
    Hybrid h(g_backing);
    p = h.data(true); // borrows h.m_own, which dies with h
  }
  sink = *p;
  return 0;
}
