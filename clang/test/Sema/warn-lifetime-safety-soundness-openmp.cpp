// RUN: %clang_cc1 -fsyntax-only -std=c++20 -fopenmp-simd -Wlifetime-safety-soundness -Wno-unused -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -Wno-source-uses-openmp -Wlifetime-safety-soundness -Wno-unused -verify=noomp %s
//
// The second run has OpenMP disabled: the pragmas are ignored, so every refusal
// disappears. What does NOT disappear is the real hazard in the loop body -- the
// loop is still a loop -- so exactly that one is expected there.

#include "Inputs/lifetime-analysis.h"
using std::string;

// OpenMP is outside the safe programming model, for two different reasons.
//
// A DIRECTIVE's data-sharing clauses copy objects the analysis does not track
// ('private'/'firstprivate' give each thread its own object, so a borrow of the
// original names storage the body never touches), and the body may run
// concurrently -- so a borrow's validity stops following from the sequential
// control flow the analysis reasons about.
//
// A 'declare reduction' is worse than unmodeled: its combiner and initializer
// are expressions hanging off the DECLARATION, not statements in a function
// body, so no CFG is ever built for them and nothing in them is analyzed. A
// whole use-after-free written in an initializer is invisible -- which is what
// this refusal is really for.
//
// The directive's body is still walked, so a hazard written there keeps its
// precise diagnostic; the refusal covers what the clauses and the concurrency
// hide, not the body.

volatile char sink;

struct Acc {
  int v;
};

// The reported shape: the whole hazard lives in the initializer.
#pragma omp declare reduction(myred:Acc                                        \
                              : omp_out.v += omp_in.v)                         \
    initializer(omp_priv = Acc{({                                              \
                  const char *p;                                               \
                  {                                                            \
                    string s("a long heap string exceeding the sso buffer");   \
                    p = s.data();                                              \
                  }                                                            \
                  (int)*p;                                                     \
                })})
// expected-warning@-10 {{'declare reduction' is not modeled by lifetime safety analysis}}

void uses_reduction() {
  Acc a{0};
#pragma omp simd reduction(myred : a) // expected-warning {{OpenMP directive is not modeled}}
  for (int i = 0; i < 4; ++i)
    a.v += i;
  sink = (char)a.v;
}

// A plain directive is refused too.
void plain_directive() {
  int t = 0;
#pragma omp simd // expected-warning {{OpenMP directive is not modeled}}
  for (int i = 0; i < 4; ++i)
    t += i;
  sink = (char)t;
}

// The body is still analyzed: a hazard written there keeps its own diagnostic
// alongside the refusal, rather than being swallowed by it.
void body_still_analyzed() {
  const char *p;
#pragma omp simd // expected-warning {{OpenMP directive is not modeled}}
  for (int i = 0; i < 4; ++i) {
    {
      string s("a long heap string exceeding the sso buffer");
      p = s.data(); // expected-warning {{does not live long enough}}
                    // noomp-warning@-1 {{does not live long enough}}
    }        // expected-note {{destroyed here}}
             // noomp-note@-1 {{destroyed here}}
    sink = *p; // expected-note {{later used here}}
               // noomp-note@-1 {{later used here}}
  }
}

// Code with no OpenMP construct is untouched -- and with OpenMP disabled the
// pragmas are ignored, so the second RUN line expects nothing at all.
void no_openmp_here() {
  string s("a long heap string exceeding the sso buffer");
  sink = s.data()[0]; // no-warning
}
