// DESC: the [[gsl::Pointer]] spelling of owner_nested_borrow_member.cpp. A view
// is meant to hold borrows, so the owner-member rule does not apply; what should
// have covered it is the unknown-ownership report on the plain sub-aggregate in
// the initializer. That report skipped itself whenever the aggregate initialized
// a declaration, on the assumption that VisitDeclStmt covers the declaration --
// which it does not when the DECLARED type is annotated. The identical
// sub-aggregate in an escaping temporary (`return View{Raw{p}};`) was reported,
// so the two spellings of one construct disagreed.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct Raw {
  std::vector<int> *v;
  ~Raw();
};
Raw::~Raw() { v->push_back(42); }

struct [[gsl::Pointer]] View {
  Raw r;
};

volatile int sink;

int main() {
  std::vector<int> v;
  v.push_back(7);
  int *p;
  {
    View g{{&v}};
    p = &v[0];
  } // ~View -> ~Raw -> push_back reallocates
  sink = *p;
  return 0;
}
