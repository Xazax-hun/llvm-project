// DESC: a C++23 STATIC `operator[]` drops its [[clang::lifetimebound]]. An operator call
// written with object syntax carries the object expression as argument 0, while a static
// operator's own parameters start at 0, so the object has to be dropped or every parameter
// shifts by one. That drop was gated on `operator()` by name, and C++23 allows `operator[]`
// to be static too -- so `r[k]` claimed its result borrowed `r`, the Registry object, which
// outlives everything, instead of `k`, whose buffer dies at the end of the block.
//
// The annotation here is truthful and is the canonical use of lifetimebound: the returned
// view does refer to `key`. Nothing else had cause to complain, so the analysis was silent
// and so was -Wdangling, which had the same off-by-one from the other direction (it blamed
// the object, and reported `return r[longLivedRef]` as returning the address of the local
// `r`).
//
// Asking whether the callee is a static MEMBER, rather than naming the operators, covers any
// future static operator. `isStatic()` alone would not: a file-static free operator is
// static too and its argument 0 is a real parameter.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Registry {
  // Looks up nothing; it just views the key it is handed. The view borrows `key`, which is
  // exactly what the annotation says.
  static std::string_view operator[](const std::string &key [[clang::lifetimebound]]) {
    return std::string_view(key);
  }
};

int main() {
  Registry r;
  std::string_view sv;
  {
    std::string k(64, 'x'); // heap-allocated buffer
    sv = r[k];
  } // `k` dies here, and with it the buffer `sv` points into
  for (char c : sv)
    sink = c;
  return 0;
}
