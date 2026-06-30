// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wlifetime-safety-coroutine -verify %s

// Minimal coroutine machinery.
namespace std {
template <class Ret, class...> struct coroutine_traits {
  using promise_type = typename Ret::promise_type;
};
template <class Promise = void> struct coroutine_handle;
template <> struct coroutine_handle<void> {
  static coroutine_handle from_address(void *) noexcept;
  void *address() const noexcept;
};
template <class Promise> struct coroutine_handle {
  static coroutine_handle from_promise(Promise &) noexcept;
  static coroutine_handle from_address(void *) noexcept;
  void *address() const noexcept;
  void resume() const;
  void destroy() const;
  operator coroutine_handle<>() const noexcept;
};
struct suspend_always {
  bool await_ready() const noexcept { return false; }
  void await_suspend(coroutine_handle<>) const noexcept {}
  void await_resume() const noexcept {}
};
} // namespace std

struct Task {
  struct promise_type {
    Task get_return_object();
    std::suspend_always initial_suspend() noexcept;
    std::suspend_always final_suspend() noexcept;
    void return_void();
    void unhandled_exception();
  };
};

// A coroutine is rejected: its body can resume after a borrowed argument's
// temporary is destroyed, which the analysis does not model.
Task coro(int x) { // expected-warning {{coroutines are not modeled by lifetime safety analysis}}
  co_return;
}

Task coro_with_await() { // expected-warning {{coroutines are not modeled by lifetime safety analysis}}
  co_await std::suspend_always{};
}

// Control: an ordinary function is not flagged.
int normal(int x) { return x; } // no-warning
