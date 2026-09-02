// DESC: a [[clang::noescape]] borrow stored into another parameter's object through a
// POINTER. The escape is detected by looking for a parameter PLACEHOLDER among the loans the
// store's container holds. Through a reference that works: a reference has no storage of its
// own, so its lvalue origin IS the referred-to object's and the placeholder is right there.
// Through a pointer it did not: a pointer variable HAS storage, so its lvalue origin carries
// a loan naming that variable while the caller's object lives on the POINTEE origin -- the
// store looked like it targeted the local pointer, and the lying noescape went unreported,
// while the same store through a reference one line away was caught. An arrow access stores
// into what the base points at, so the container is the pointee. Neither `.data()` nor the
// escape machinery was at fault; only the receiver's form. `this` is unaffected: the model
// already gives it the object's origin rather than a pointer's.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <cstddef>

volatile char sink;

struct [[gsl::Owner]] Node {
  Node() : d(nullptr) {}
  char at(std::size_t i) const { return d[i]; }
  friend void fill(Node *n [[clang::noescape]], std::string_view v [[clang::noescape]]);
private:
  const char *d;
};

void fill(Node *n [[clang::noescape]], std::string_view v [[clang::noescape]]) { n->d = v.data(); }

int main() {
  Node n;
  { std::string s("hello world hello world hello world!!!!"); fill(&n, s); }
  sink = n.at(0);
  return 0;
}
