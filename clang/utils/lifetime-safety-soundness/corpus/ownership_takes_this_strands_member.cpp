// DESC: a member function declared `ownership_takes` on its implicit object that
// parks a borrow of a LOCAL in a member and then hands the object off to be
// destroyed. The annotation had been treated as equivalent to a destructor, so the
// field facts a destructor may skip -- nothing can read a member once ~T has returned
// -- were skipped here too.
//
// That is wrong: such a function CAUSES the destruction, it is not the destruction.
// The destructor runs after it and reads the member, which is exactly this bug. The
// same code without the annotation was reported all along.
//
// The destruction must be DEFERRED for the bug to bite: a `delete this` inside the
// function runs the destructor while the local is still alive. Deferring it -- the
// release idiom's `ensureOnMainThread([this]{ delete this; })` -- is what leaves the
// member pointing at a dead frame.
//
// Only the OBJECT's own storage stays exempt from the deallocation report; that is
// what the annotation licenses, and what the canonical `delete this` needs.
//
// The ASan compiler is the system clang, which HAS `ownership_takes` but rejects an
// index naming the implicit object, so it cannot compile the annotated form. The
// attribute changes no runtime behaviour, so drop it there.
// FLAGS: -Wno-lifetime-safety-naked-delete -Wno-lifetime-safety-const-subversion -Wno-lifetime-safety-unannotated-indirection
// EXPECT-ASAN: stack-use-after-return
#if __has_warning("-Wlifetime-safety-soundness")
#define TAKES_THIS __attribute__((ownership_takes(RC, 1)))
#else
#define TAKES_THIS
#endif

volatile char g_sink;

struct Misbehaved;
static Misbehaved *g_pending = nullptr;

static void logReason(const char *r) { g_sink = r[0]; }

struct Misbehaved {
  mutable unsigned count{1};
  const char *deathReason{nullptr};

  // Runs after `deref` has returned, and reads the member.
  ~Misbehaved() {
    if (deathReason)
      logReason(deathReason);
  }

  void deref() const TAKES_THIS {
    if (--count)
      return;
    char reason[64] = "refcount reached zero";
    const_cast<Misbehaved *>(this)->deathReason = reason;
    // Deferred destruction: `reason` is gone by the time the destructor runs.
    g_pending = const_cast<Misbehaved *>(this);
  }
};

int main() {
  auto *m = new Misbehaved;
  m->deref();
  delete g_pending; // ~Misbehaved reads `deathReason`, a borrow of deref's frame
  return 0;
}
