// DESC: a coroutine reads a by-reference parameter after the argument is gone
// EXPECT-ASAN: stack-use-after-scope
#include <coroutine>
struct task {
  struct promise_type {
    int val = 0;
    task get_return_object() {
      return task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_value(int v) { val = v; }
    void unhandled_exception() {}
  };
  std::coroutine_handle<promise_type> h;
  int get() {
    h.resume();
    return h.promise().val;
  }
  ~task() {
    if (h)
      h.destroy();
  }
};
task coro(const int &x) { // the reference parameter lives in the coroutine frame
  co_return x;            // read after the temporary argument is destroyed
}
int main() {
  task t = coro(42); // 42 is a temporary, gone after the call expression
  return t.get();
}
