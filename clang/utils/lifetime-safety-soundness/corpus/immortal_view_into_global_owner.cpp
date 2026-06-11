// DESC: a [[clang::lifetime_immortal]] accessor returns a std::string_view into
// the CONTENTS of a mutable global std::vector<std::string> (`return
// g_table[i];`). The immortal attribute promises the *result* outlives callers,
// but a global owner's heap buffer is not immortal: growing the vector
// reallocates it, so the element strings move and the view dangles. The borrow
// is a loan rooted at the global owner regardless of the attribute.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <vector>

static std::vector<std::string> g_table;

[[clang::lifetime_immortal]] std::string_view get(int i) { return g_table[i]; }

int put(std::string s) {
  g_table.push_back(s);
  return static_cast<int>(g_table.size()) - 1;
}

int main() {
  int id = put("hello");
  std::string_view name = get(id); // borrows into g_table[id]'s buffer
  for (int i = 0; i < 1000; ++i)
    put("another string that grows the table well beyond its capacity now");
  volatile char c = name.size() ? name[0] : 0; // use-after-free
  (void)c;
  return 0;
}
