// DESC: a std::string_view borrows a mutable global std::string's heap buffer;
// the global is then reallocated from a SEPARATE function (invisible to the
// intra-procedural invalidation pass), and the dangling view is read via a
// MEMBER CALL (`sv.front()`). The borrow-from-mutable-global rule exempts the
// permitted `global.method()` form by recognizing a member-call receiver, but
// the exemption was too broad: it fired for any stable-designation receiver,
// including a local view that merely borrows the global -- so a member-call use
// of the dangling view was silently exempted. Found by the multi-agent bypass
// hunt (Agent B); a regression introduced when the receiver exemption landed.
// The fix restricts the exemption to a receiver that IS the global owner itself.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

std::string g = "this is a long string exceeding the sso buffer length 1234567";

void mutate() {
  g.clear();
  g.shrink_to_fit();
  g.resize(2); // reallocates g's buffer -> any view into it dangles
}

int main() {
  std::string_view sv = g; // view borrows the global owner's heap buffer
  mutate();                // cross-function realloc, invisible intra-procedurally
  return sv.front();       // use-after-free, read through a member call
}
