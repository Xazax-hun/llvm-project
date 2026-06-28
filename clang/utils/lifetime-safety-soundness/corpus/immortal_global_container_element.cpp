// DESC: a [[clang::lifetime_immortal]] accessor returns a borrow extracted from
// a GLOBAL container of indirection (std::vector<std::string_view>). The element
// the view is read from lives in the global container's storage, but the view's
// *pointee* (the buffer pushed in) is not immortal, so the immortal promise is a
// lie. The container of indirection is itself banned by the model; flagging any
// use of such a global at the use site closes this (a global's declaration may
// be outside the analyzed region, so the use site is where it must be caught).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <vector>

std::vector<std::string_view> g_registry;

[[clang::lifetime_immortal]] std::string_view firstEntry() {
  return g_registry.front(); // use of a global container of indirection
}

int main() {
  {
    std::string entry("a heap-allocated string registered only transiently!!");
    g_registry.push_back(entry); // view into entry's heap buffer stored globally
  }                              // entry destroyed -> buffer freed
  std::string_view sv = firstEntry(); // immortal promise trusted
  volatile char c = sv[0];            // use-after-free
  (void)c;
  return 0;
}
