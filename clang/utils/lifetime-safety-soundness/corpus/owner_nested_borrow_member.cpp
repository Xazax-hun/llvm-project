// DESC: a [[gsl::Owner]] whose public member is a plain struct that itself holds
// a borrow. The owner-public-borrow-member check tested the FIELD's own type
// (pointer / reference / gsl::Pointer), so wrapping the offending member in one
// plain struct silenced it -- while the direct member form was reported. The
// guard's member destructor then reallocates the borrowed vector, and the
// annotation kept the owner's contents opaque, so nothing saw the borrow.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct Raw {
  std::vector<int> *v;
  ~Raw();
};
Raw::~Raw() { v->push_back(42); } // reallocates the borrowed buffer

struct [[gsl::Owner]] Guard {
  Raw r;
};

volatile int sink;

int main() {
  std::vector<int> v;
  v.push_back(7);
  int *p;
  {
    Guard g{{&v}};
    p = &v[0]; // borrow into the vector's buffer
  }            // ~Guard -> ~Raw -> push_back reallocates
  sink = *p;   // heap-use-after-free
  return 0;
}
