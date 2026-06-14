// DESC: a [[gsl::Pointer]] wrapper reaches a std::vector owner through a raw
// POINTER member. A lifetimebound accessor hands out a reference into the owner;
// a later non-const method reallocates the owner through the pointer member.
// recordContainsMutableOwner did not descend through pointer/reference members,
// so the wrapper was not seen as "containing a mutable owner" and the non-const
// call did not assumed-invalidate the live borrow. Now it does.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

struct [[gsl::Pointer]] Wrap {
  std::vector<int> *v;
  Wrap(std::vector<int> *p [[clang::lifetime_capture_by(this)]]) : v(p) {}
  int &at0() [[clang::lifetimebound]] { return (*v)[0]; }
  void grow() {
    for (int i = 0; i < 1000; ++i)
      v->push_back(i); // reallocates *v
  }
};

volatile int g;

int main() {
  std::vector<int> data;
  data.push_back(42);
  Wrap w(&data);
  int &r = w.at0(); // borrow into data, handed out through the wrapper
  w.grow();         // reallocates data through w.v -> r dangles
  g = r;            // heap-use-after-free
  return (int)g;
}
