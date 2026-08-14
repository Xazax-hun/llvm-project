// DESC: invalidation through a pointer to an INCOMPLETE type -- the C opaque-handle
// idiom. Assumed invalidation answered "can the callee reach a mutable owner through
// this parameter" from the pointee's type, and a forward-declared record answers
// nothing: `recordContainsMutableOwner` needs a definition. Both annotations here are
// truthful and every body is present in the TU.
//
// What made this worse than the `void *` case is that it was ORDER-DEPENDENT. Moving
// `struct Session`'s definition above `run` made the identical code fire, because by
// then the type was complete. Deferring the whole analysis to TU end also caught it
// (-fexperimental-lifetime-safety-tu-analysis reported it), so whether the hazard was
// visible depended on where the analysis ran rather than on the code. In a normal
// project the using TU only ever sees the forward declaration, so the whole
// opaque-handle API surface was silently unchecked -- and not diagnosable as a missing
// annotation, since nothing was missing.
//
// The fix treats an incomplete pointee like `void`: it cannot be shown to be owner-free,
// so it is not assumed to be.
// EXPECT-ASAN: heap-use-after-free
volatile char sink;

struct Session; // opaque at the point `run` is analyzed

void session_reset(Session *s [[clang::noescape]]);
const char *session_text(const Session *s [[clang::lifetimebound]]);

static void run(Session *s [[clang::noescape]]) {
  const char *p = session_text(s); // borrow into the session's string
  session_reset(s);                // reallocates it
  sink = p[0];                     // heap-use-after-free
}

// Completed only after `run` -- which is why the check cannot depend on completeness
// at the point of analysis.
#include <string>
struct Session {
  std::string t{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
};

void session_reset(Session *s [[clang::noescape]]) { s->t = std::string("b"); }
const char *session_text(const Session *s [[clang::lifetimebound]]) {
  return s->t.c_str();
}

int main() {
  Session s;
  run(&s);
  return 0;
}
