// DESC: a virtual override silently changes the lifetimebound FLAVOUR. The base
// declares '[[clang::lifetimebound(pointee)]]' and truthfully returns what it
// refers to; the override declares plain '[[clang::lifetimebound]]' and truthfully
// returns storage inside the object. No annotation lies -- each body agrees with
// its own -- but a virtual call is checked against the BASE's declaration, so the
// object binding is dropped while dynamic dispatch really does hand back the
// object's own storage, and the borrow outlives the object unreported. The same
// conflict across a REDECLARATION was already diagnosed; across an override it was
// not, though it is worse there because dispatch is dynamic and the call site
// cannot know which flavour applies. Found by the multi-agent bypass hunt on the
// new annotation. Closed by diagnosing the unsound direction (base pointee,
// override object-bound) alongside the existing override checks; the reverse only
// over-approximates and stays allowed.
// EXPECT-ASAN: stack-use-after-scope
#if __has_warning("-Wlifetime-safety-soundness")
#define LB [[clang::lifetimebound]]
#define LB_POINTEE [[clang::lifetimebound(pointee)]]
#else
#define LB
#define LB_POINTEE
#endif

volatile char sink;
char g_backing[8] = {'g', 0};

struct [[gsl::Pointer(char)]] Base {
  const char *m_ref;
  Base(const char *r) : m_ref(r) {}
  virtual ~Base() = default;
  virtual const char *data() const LB_POINTEE { return m_ref; }
};

struct [[gsl::Pointer(char)]] Derived : Base {
  char m_own[8];
  Derived(const char *r) : Base(r) {
    m_own[0] = 'z';
    m_own[1] = 0;
  }
  const char *data() const LB override { return m_own; }
};

int main() {
  const char *p;
  {
    Derived d(g_backing);
    p = static_cast<Base &>(d).data(); // dispatches to Derived::data -> &d.m_own
  }
  sink = *p;
  return 0;
}
