// DESC: the borrow two records below a [[gsl::Owner]] rather than one. Recursing
// only a single level would have left this silent; the ownership question is
// asked of the member type as a whole, which already answers transitively.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct Raw {
  std::vector<int> *v;
  ~Raw();
};
Raw::~Raw() { v->push_back(42); }

struct Mid {
  Raw r;
};

struct [[gsl::Owner]] Guard {
  Mid m;
};

volatile int sink;

int main() {
  std::vector<int> v;
  v.push_back(7);
  int *p;
  {
    Guard g{{{&v}}};
    p = &v[0];
  }
  sink = *p;
  return 0;
}
