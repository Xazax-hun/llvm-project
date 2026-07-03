// DESC: a const method mutates its [[gsl::Owner]]'s heap buffer by invoking a
// std::function data member whose closure captured [this] in the constructor.
// Assumed-invalidation only fires for non-const calls, and the [this] capture
// happened in a different function, so invoking the const callable member from
// a const method used to reallocate the buffer undetected while a lifetimebound
// string_view into it was live. Invoking a callable data member must be treated
// as a possible mutation of the enclosing object regardless of const.
// EXPECT-ASAN: heap-use-after-free
#include <functional>
#include <string>
#include <string_view>

static volatile int sink;

struct [[gsl::Owner(std::string)]] MyOwner {
private:
  std::string buf{std::string(100, 'a')}; // heap-allocated (beyond SSO)
  std::function<void()> grower;

public:
  MyOwner() { grower = [this] { buf.append(200, 'b'); }; } // captures [this]
  std::string_view view() const [[clang::lifetimebound]] { return buf; }
  void poke() const { grower(); } // const, yet reallocates buf via the closure
};

int main() {
  MyOwner o;
  std::string_view sv = o.view(); // borrow into buf's heap storage
  o.poke();                       // reallocates buf -> sv dangles
  sink = sv[0];                   // heap-use-after-free
  return 0;
}
