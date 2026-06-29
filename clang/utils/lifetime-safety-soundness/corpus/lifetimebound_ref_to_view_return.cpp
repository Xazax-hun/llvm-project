// DESC: std::max over a view type returns `const std::string_view&` -- a
// reference TO a view (two levels of indirection). Its lifetimebound flow
// constrains only the top-level (reference) origin, so the inner level (the
// view's own borrow into the temporary std::string's buffer) is dropped; the
// result origin is left empty and only the lost-loan heuristic catches it.
// A control-flow merge supplying a valid loan on the other path then masks
// lost-loan. Seeding the dropped inner level with an Unknown loan (which
// survives joins) makes the use reliably trip lost-loan.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <algorithm>

volatile char sink;

int main(int argc, char **) {
  std::string keep = "a kept long heap string ttttttttttttttttttttttttttttt";
  std::string_view sv;
  if (argc > 100)
    sv = std::string_view(keep); // valid loan on one path...
  else
    sv = std::max(std::string_view(keep),
                  std::string_view(std::string(
                      "temp long heap string uuuuuuuuuuuuuuuuuuuuuuuuuuuu")));
  sink = sv.data()[0]; // use-after-free: the temporary string's buffer is freed
  return sink == 0 ? 1 : 0;
}
