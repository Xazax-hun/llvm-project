// DESC: a struct caches a string_view into its own std::string member, but the
// borrow is laundered through a lifetimebound helper (not a direct field-to-
// field assignment). A later method call reallocates the member, leaving the
// self-referential view dangling. Detection must be loan-based (the borrow is
// invisible in the assignment's AST shape).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <cstddef>

// Lifetimebound: the returned view borrows `s`.
static std::string_view prefix(const std::string &s [[clang::lifetimebound]],
                               std::size_t n) {
  return std::string_view(s.data(), n);
}

struct [[gsl::Owner]] Cache {
  std::string buffer = std::string(50, 'a');
  void cacheToken() { token = prefix(buffer, 3); } // laundered self-referential borrow
  std::size_t grow() {
    buffer.append(10000, 'b');                      // reallocates buffer -> token dangles
    return token.size() ? token[0] : 0;             // use-after-free
  }

private:
  std::string_view token; // points into `buffer`
};

int main() {
  Cache c;
  c.cacheToken();
  return static_cast<int>(c.grow());
}
