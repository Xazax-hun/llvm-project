// DESC: a member function marked [[clang::lifetime_immortal]] returns a view into
// this->data (heap storage owned by the object), not immortal storage. Unlike
// [[clang::lifetimebound]] (whose body is verified), the immortal promise was
// trusted unverified, so the call result was not bound to the object's lifetime
// and a caller kept it past the object's destruction. Found by the 5th
// multi-agent bypass hunt (D3); fixed by verifying the immortal body.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
struct Cache {
  std::string data = "a long heap string aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  [[clang::lifetime_immortal]] std::string_view view() const { return data; }
};
char run() { std::string_view v; { Cache c; v = c.view(); } return v[0]; }
int main() { volatile char ch = run(); (void)ch; return 0; }
