// DESC: an ARRAY of scope guards. Destroying a guard that holds a borrow is
// modelled as an assumed invalidation, since the analysis cannot see what the
// destructor does -- but the check began by asking the destroyed type for its
// CXXRecordDecl, and an array type has none, so the whole check bailed out. An
// array of guards was therefore silent while the byte-identical scalar
// `Grower g{&vec};` reported. The origin tree already shares one origin across an
// array's elements, so the element type is what decides the hazard, exactly as for
// a single guard.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile char g_sink = 0;

struct [[gsl::Pointer]] Grower {
  std::vector<int> *v;
  ~Grower() { v->resize(4000); } // reallocates the borrowed vector
};

int main() {
  std::vector<int> vec(1, 5);
  int *p = &vec[0];
  {
    Grower arr[1] = {Grower{&vec}};
    (void)arr;
  }                      // ~Grower runs here and reallocates vec
  g_sink = (char)*p;     // heap-use-after-free
  return 0;
}
