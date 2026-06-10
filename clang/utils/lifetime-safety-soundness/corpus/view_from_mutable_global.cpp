// DESC: a string_view is created from a mutable global string; mutating the
// global elsewhere reallocates its buffer and invalidates the view, which the
// intra-procedural analysis cannot see (the mutation is in another function)
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

std::string g = std::string(50, 'a'); // heap-allocated (beyond SSO)

void grow() { g.append(10000, 'b'); } // reallocates g, in another function

int f() {
  std::string_view sv = g; // sv borrows g's heap buffer
  grow();                  // reallocation hidden from f
  return sv.size() ? sv[0] : 0; // reads g's freed old buffer
}

int main() { return f(); }
