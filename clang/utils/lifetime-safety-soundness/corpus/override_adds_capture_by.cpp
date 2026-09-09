// DESC: a virtual override ADDING [[clang::lifetime_capture_by]] that the base does
// not declare. A call is checked against the statically resolved callee, so the
// contract the override adds is invisible to every caller dispatching through the
// base: the base says the callee merely borrows for the duration of the call, so the
// caller hands over an argument it must not let escape, and the override parks it in
// the object.
//
// Nothing is reported at the call site, because it is checked against the base; and
// nothing in the override either, because its body does exactly what its own
// annotation permits. Adding [[clang::lifetimebound]] in an override was already
// refused for the same reason.
// FLAGS: -Wno-lifetime-safety-unannotated-indirection
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink;

struct Observer {
  virtual ~Observer() = default;
  // Declares no capture: callers may pass a borrow that dies right after the call.
  virtual std::string_view onEvent(std::string_view payload [[clang::lifetimebound]]) {
    return payload;
  }
};

struct [[gsl::Pointer(char)]] CachingObserver : Observer {
  std::string_view last;
  CachingObserver(std::string_view initial [[clang::lifetimebound]]) : last(initial) {}
  std::string_view onEvent(std::string_view payload [[clang::lifetimebound]]
                           [[clang::lifetime_capture_by(this)]]) override {
    last = payload;
    return payload;
  }
  char lastFirst() const { return last[0]; }
};

// Checked against Observer::onEvent, which promises no capture.
static void publish(Observer &o [[clang::noescape]],
                    std::string_view payload [[clang::noescape]]) {
  o.onEvent(payload);
}

static const char kImmortal[] = "immortal storage long enough to never be SSO";

int main() {
  CachingObserver co{kImmortal};
  {
    std::string tmp("a heap string long enough to never be SSO at all here");
    publish(co, tmp); // the caller obeys noescape and drops `tmp`
  }
  g_sink = co.lastFirst();
  return 0;
}
