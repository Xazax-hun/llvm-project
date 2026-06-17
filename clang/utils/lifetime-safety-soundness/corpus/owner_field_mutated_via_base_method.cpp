// DESC: a std::vector owner field of a DERIVED class is mutated (reallocated)
// through a method inherited from / dispatched on a BASE class. The assumed-
// invalidation check tested recordHasGslOwnerField on the STATIC receiver type,
// but the implicit object argument of the base method carries a derived-to-base
// cast, so its static type is the base -- which declares no owner field. The
// invalidation was skipped and the string_view into the derived's vector stayed
// tracked-as-valid across the reallocation: a silent heap-use-after-free. The
// receiver type is now recovered with IgnoreImpCasts (most-derived receiver).
// EXPECT-ASAN: heap-use-after-free
#include <string_view>
#include <vector>
struct Base {
  virtual void grow() = 0;
  void doGrow() { grow(); } // receiver static type Base: no owner field
  virtual ~Base() = default;
};
struct Widget : Base {
  std::vector<char> buf = std::vector<char>(8, 'x');
  void grow() override {
    for (int i = 0; i < 10000; i++)
      buf.push_back('z');
  }
  __attribute__((noinline)) int bug() {
    std::string_view sv(buf.data(), buf.size());
    doGrow();                            // reallocs buf via the base method
    return sv.size() ? (int)sv[0] : 0;   // reads freed heap
  }
};
int main() { Widget w; return w.bug(); }
