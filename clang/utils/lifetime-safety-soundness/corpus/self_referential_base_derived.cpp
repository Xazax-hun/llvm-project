// DESC: a [[gsl::Pointer]] BASE subobject holds a std::string_view that is bound
// (via a [[clang::lifetime_capture_by(this)]] helper) to a std::string member of
// the [[gsl::Owner]] DERIVED class. The complete object is self-referential
// across the base/derived boundary, and destruction order makes it fatal: when a
// Derived object is destroyed, ~Derived frees `data` first, then ~Base reads the
// dangling `cached` view. The self-referential check keys on the most-derived
// receiver type, so a base-subobject view bound to a derived member is caught.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char g_sink;

struct [[gsl::Pointer]] Base {
  std::string_view cached;
  void observe(std::string_view v [[clang::lifetime_capture_by(this)]]) {
    cached = v;
  }
  ~Base() { g_sink = cached.empty() ? '?' : cached[0]; }
};

struct [[gsl::Owner]] Derived : Base {
  std::string data;
  Derived()
      : data("a long heap allocated derived data string xxxxxxxxxxxxxx") {
    observe(data); // bind base view to derived member
  }
};

int main() {
  { Derived d; } // ~Derived frees data, then ~Base reads cached -> UAF
  return (int)g_sink;
}
