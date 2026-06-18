// DESC: a method returns a closure capturing `[this]` (a borrow of the enclosing
// object) plus a benign co-captured lifetimebound-param pointer. The `[this]`
// capture was not modeled, so the closure carried no object loan; the benign
// capture kept the closure origin non-empty, masking lost-loan, while the
// escaping closure's dangling `this` read stayed silent. The `[this]` capture
// now flows the `this` origin into the lambda, and the unannotated escape of
// `this` via the closure return is surfaced (intra-TU lifetimebound suggestion,
// part of the soundness model). The object is deleted before the closure runs.
// EXPECT-ASAN: heap-use-after-free
#include <cstdio>
volatile int sink;
struct W {
  int x;
  W(int v) : x(v) {}
  auto getter(int &keep [[clang::lifetimebound]]) {
    return [this, kp = &keep]() { sink = x; }; // captures this (dangles) + benign
  }
};
int main() {
  int keep = 9;
  W *w = new W(424242);
  auto f = w->getter(keep);
  delete w;   // object freed; closure still holds a dangling `this`
  f();        // reads w->x -> heap use-after-free
  return sink ? 1 : 0;
}
