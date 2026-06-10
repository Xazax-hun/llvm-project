// DESC: a struct caches a string_view into its own std::string member; a later
// method call reallocates that member, leaving the self-referential view
// dangling. The struct is [[gsl::Owner]]-annotated (with a private view field)
// to satisfy the safe model's unknown-ownership rule, which makes it opaque --
// so the self-reference is invisible to the intra-procedural analysis unless
// the self-referential store itself is flagged.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <cstddef>

struct [[gsl::Owner]] Cache {
  std::string buffer = std::string(50, 'a'); // heap-allocated (beyond SSO)
  void cacheToken() { token = buffer; }       // self-referential borrow
  std::size_t grow() {
    buffer.append(10000, 'b');                // reallocates buffer -> token dangles
    return token.size() ? token[0] : 0;       // use-after-free
  }

private:
  std::string_view token; // points into `buffer`
};

int main() {
  Cache c;
  c.cacheToken();
  return static_cast<int>(c.grow());
}
