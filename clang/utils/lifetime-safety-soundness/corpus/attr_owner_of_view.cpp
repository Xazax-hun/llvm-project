// DESC: a struct mislabeled [[gsl::Owner(std::string_view)]] -- declaring (via
// the attribute's optional type argument) that it OWNS a std::string_view, which
// is itself a view. It only holds a borrow. Wrapped in a std::unique_ptr and
// returned with its view member bound to a local, the dangling view escapes.
// The owner-of-indirection check now inspects the attribute's deref type (not
// only template arguments), so the self-contradictory "owner of a view" is
// rejected.
// EXPECT-ASAN: heap-use-after-free
#include <memory>
#include <string>
#include <string_view>

struct [[gsl::Owner(std::string_view)]] CachedSlice {
  std::string_view sv;
};

std::unique_ptr<CachedSlice> build() {
  std::string local = "this is a long heap allocated local string xyz1234";
  auto p = std::make_unique<CachedSlice>();
  p->sv = std::string_view(local); // store a borrow into local
  return p;                        // local dies; p->sv dangles
}

volatile char sink;

int main() {
  auto p = build();
  sink = p->sv[0]; // heap-use-after-free
  return sink == 0 ? 1 : 0;
}
