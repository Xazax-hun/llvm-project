// DESC: a [[gsl::Pointer]] wrapper reaching a std::vector owner through a raw
// pointer member is passed BY VALUE to a free function that reallocates the owner
// through it. The copy still aliases the same owner, so a borrow taken directly
// from the owner dangles. The gsl::Pointer pointee-invalidation was added only to
// the method-receiver path; this exercises the function-argument (param-loop)
// path.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct [[gsl::Pointer]] W {
  std::vector<int> *v;
  W(std::vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
};

void grow(W w [[clang::noescape]]) {
  for (int i = 0; i < 1000; ++i)
    w.v->push_back(i); // reallocates the aliased owner
}

volatile int g_sink;

int main() {
  std::vector<int> data;
  data.push_back(1);
  W w(&data);
  int &r = data[0]; // borrow taken directly from the owner (aliases w.v)
  grow(w);          // reallocates data through the by-value wrapper -> r dangles
  g_sink = r;       // heap-use-after-free
  return g_sink;
}
