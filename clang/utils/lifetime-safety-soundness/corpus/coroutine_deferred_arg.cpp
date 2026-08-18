// DESC: a coroutine takes a borrow-holding parameter (truthfully [[noescape]] --
// the reference is only read, never stored). Because initial_suspend suspends,
// the body runs on resume(), AFTER the argument temporary was destroyed at the
// end of the call's full-expression; the deferred read then dangles. The
// analysis modeled the call as ordinary (the temporary outliving the call), so
// the deferred parameter use was never connected to the expired temporary.
// Coroutines are now rejected as an unsupported construct.
// EXPECT-ASAN: stack-use-after-scope
#include <coroutine>
#include <string>

volatile char g_sink;

struct Task {
  struct promise_type {
    Task get_return_object() { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
  };
  std::coroutine_handle<promise_type> h;
  void resume() { h.resume(); }
  ~Task() { if (h) h.destroy(); }
};

Task coro(const std::string &s [[clang::noescape]]) {
  g_sink = s.empty() ? '?' : s[0]; // runs on resume(), reads dangling reference
  co_return;
}

int main() {
  Task t = coro(std::string("a long heap allocated string past the SSO buffer"));
  t.resume(); // body runs now -- the argument temporary is already gone
  return 0;
}
