// DESC: a whole heap use-after-free written in a VLA's size expression, reached through
// type sugar. A VLA bound is evaluated where the type is written and can carry arbitrary
// side effects, so it belongs in the CFG -- but the CFG found it by casting the variable's
// type to ArrayType, which sees nothing when the array sits behind sugar. With
// `__typeof__(char[...])` the bound was absent from the CFG entirely, so nothing in it was
// analyzed. The plain spelling `char arr[...]` was always caught, and typedef/using are
// caught where the TYPE is declared -- so this was one spelling of the same construct
// falling through. `__typeof__` of an EXPRESSION is correctly silent: that operand is
// unevaluated, so nothing runs.
// FLAGS: -Wno-unused -Wno-vla-cxx-extension -Wno-deprecated-volatile
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;
std::string *gp;
std::string_view gsv;
volatile int gn = 3;

int main() {
  int n = gn;
  __typeof__(char[( gp = new std::string(66, 'c'),
                    gsv = std::string_view(*gp),
                    delete gp,
                    sink = gsv[0],      // heap-use-after-free
                    n )]) arr;
  arr[0] = 1;
  sink = arr[0];
  return 0;
}
