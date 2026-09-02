// DESC: same root cause one level deeper, and it needs its own handling. With the
// aggregate holding the reference nested inside another aggregate, Clang does NOT
// re-target the lifetime extension to the complete-object variable: the AST leaves it
// as "extended by Field 'inner'". So even once the walk reaches the temporary, matching
// the extending decl against the variable rejects it. The storage still dies with the
// variable -- the object owning that member IS the variable -- so a subobject extension
// found inside the variable's own initializer has to count.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

static std::string mk() {
  return std::string("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
}

struct [[gsl::Pointer(char)]] Agg { const std::string &r = mk(); };
struct [[gsl::Pointer(char)]] Outer { Agg inner{}; };

int main() {
  std::string_view out;
  { Outer o{}; out = o.inner.r; }
  sink = out[0]; // heap-use-after-free
  return 0;
}
