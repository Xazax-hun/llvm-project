// DESC: a callback passed BY VALUE reallocates the caller's owner. A by-value copy of a
// closure copies its captures, and a by-REFERENCE capture is an alias to the caller's
// object rather than a copy of it -- so the callee can reallocate the very buffer the
// receiver borrows into. Nothing in the signature says so: the parameter's type is a
// plain class, not a pointer or reference, so the predicate deciding "can this call
// mutate an owner" (which only looked at pointer/reference parameters, plus gsl::Pointer
// by value) said no. The identical hazard through an explicit owner argument,
// `ws[0].go(ws)`, was reported all along, so hiding the owner inside a lambda capture was
// the whole difference. The '[[clang::noescape]]' on the callback is TRUE and beside the
// point: the callback does not outlive the call, it reallocates during it.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int sink;

struct Widget {
  int id = 7;
  template <class Fn>
  void withNotify(Fn fn [[clang::noescape]]) {   // annotation is TRUE: fn doesn't escape
    fn(id);
    sink = id;        // `this` dangles if fn() reallocated the owner
  }
};

int main() {
  std::vector<Widget> ws;
  ws.emplace_back();
  ws[0].withNotify([&ws](int i) { ws.push_back(Widget{i + 1}); });
  return 0;
}
