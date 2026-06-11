// DESC: a lambda captures a std::string BY REFERENCE and mutates it (forcing a
// reallocation) when called. A view into the string is taken before the call,
// then read after -- a use-after-invalidation. A by-reference capture gives the
// closure non-const access to the owner, so calling it is equivalent to passing
// the owner to a non-const reference parameter, but the by-ref-capture channel
// was previously not modeled. Found by the 3rd multi-agent bypass hunt (B3).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

int main() {
  std::string s = "this is already a heap allocated string buffer here";
  auto applyEdit = [&s]() {
    s += " ---- appended enough text to force a heap reallocation of s now";
  };
  std::string_view v = s; // view into s's current heap buffer
  applyEdit();            // reallocates s -> v dangles
  unsigned long acc = 0;
  for (char c : v) // read of freed memory
    acc += (unsigned char)c;
  return (int)(acc & 0x7f);
}
