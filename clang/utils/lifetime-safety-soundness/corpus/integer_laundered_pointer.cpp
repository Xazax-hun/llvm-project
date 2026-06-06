// DESC: a borrow laundered through an integer (uintptr_t) and recovered via an
// rvalue reinterpret_cast that is dereferenced directly, without ever binding a
// named pointer variable, escapes the analysis (integers carry no origin and no
// tracked pointer lvalue is created)
// EXPECT-ASAN: stack-use-after-scope
#include <cstdint>
std::uintptr_t g;
int main() {
  {
    int local = 42;
    g = reinterpret_cast<std::uintptr_t>(&local);
  }
  return *reinterpret_cast<int *>(g); // dangling; recovered as an rvalue cast
}
