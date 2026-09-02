// DESC: a whole heap use-after-free written inside an OpenMP 'declare reduction'
// initializer. The combiner and initializer are expressions hanging off the
// DECLARATION, not statements in any function body, so no CFG is ever built for them
// and nothing in them is analyzed -- the hazard is invisible, and so is the same hazard
// written in the combiner. The directive's BODY is analyzed fine (a hazard written in
// an `omp simd` loop body is reported precisely), so this was specifically the
// declaration-level construct being unreachable, not OpenMP code in general. Both the
// declaration and the directive are now refused as outside the safe programming model:
// a directive's data-sharing clauses copy objects the analysis does not track and its
// body may run concurrently.
// FLAGS: -Wno-unused -fopenmp-simd
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;
struct Acc { int v; };

#pragma omp declare reduction(myred : Acc : omp_out.v += omp_in.v) \
  initializer(omp_priv = Acc{ ({ const char* p; \
      { std::string s("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"); \
        p = s.data(); } \
      (int)*p; }) })

int main() {
  Acc a{0};
  #pragma omp simd reduction(myred:a)
  for (int i = 0; i < 4; ++i) a.v += i;
  sink = (char)a.v;
  return 0;
}
