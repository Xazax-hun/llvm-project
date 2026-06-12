// DESC: a callable that holds a by-reference borrow of an owner is invoked
// through an indirection that hides the closure from syntactic recognition --
// passed by value to a generic helper with a [[clang::noescape]] parameter, or
// stored in a std::function variable first. Invoking it reallocates the captured
// owner, dangling a view taken before the call. The fix is loan-based: the
// callable's value carries the captured owner's loan, so invalidating the
// callable argument's loans at the call site connects to the live view --
// independent of how the closure was wrapped. Found by the 6th multi-agent
// bypass hunt (E2).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <functional>

int run_named() {
  std::string text = "key=value; a reasonably long config line to force heap use";
  std::string_view tok = text;
  std::function<void()> closure = [&text] { text.resize(text.size() * 4 + 256, 'x'); };
  closure();                       // reallocates text -> tok dangles
  return tok.empty() ? 0 : tok[0]; // use-after-free
}

int main() { return run_named(); }
