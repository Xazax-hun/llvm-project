// DESC: an RAII guard created as an UNNAMED TEMPORARY. A guard's destructor can mutate the
// owner it captured -- the classic `~Grower() { vec->push_back(9); }` reallocating a vector --
// which the intra-procedural analysis cannot see, so the destruction is treated as an assumed
// invalidation of the borrows the guard carries on that owner.
//
// That was modelled only where a NAMED local's lifetime ends. A temporary is destroyed by its
// full-expression cleanup instead, which emitted the expiry and nothing else -- so forgetting
// to name the guard silenced it. The control is byte-identical apart from the name:
// `{ Grower g{&v}; }` reports, `(void)Grower{&v}.vec;` did not.
//
// Forgetting to name a scope guard is a mistake people actually make, and it usually shows up
// as the guard doing nothing. Here it also disabled the check that would have caught the
// resulting dangle.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int isink;

struct [[gsl::Pointer]] Grower {
  std::vector<int> *vec;
  ~Grower() { vec->push_back(9); }
};

int main() {
  std::vector<int> v{1, 2, 3};
  int *p = &v[0];
  (void)Grower{&v}.vec; // destroyed at the end of this full expression
  isink = *p;
  return 0;
}
