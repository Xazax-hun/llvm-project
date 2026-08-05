// DESC: a borrow held by a [[gsl::Pointer]] view is invalidated, and the dangling
// read is performed by that view's DESTRUCTOR -- an implicit use with no source
// expression. The borrow comes from a [[clang::lifetimebound]] accessor of `this`,
// so its loan is the `$this` placeholder: no issuing expression and no placeholder
// parameter. issuePendingWarnings' implicit-use branch tested only for those two
// anchors and had no final `else`, so the invalidation was detected and then emitted
// nothing. The explicit-use branch a few lines below already had exactly that
// fallback, which is why the identical code with an explicit `sink = p.v[0];` before
// scope exit was reported and this was not. Now anchored at the method whose
// implicit object the placeholder stands for.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

// A view type whose destructor reads the borrow it holds.
struct [[gsl::Pointer]] Printer {
  std::string_view v;
  ~Printer() { sink = v[0]; } // implicit use at scope exit
};

struct [[gsl::Owner]] Doc {
  std::string s;
  std::string_view text() const [[clang::lifetimebound]] { return s; }

  void run() {
    Printer p{text()};  // p.v borrows this->s
    s.assign(400, 'y'); // reallocates s -> p.v dangles
  }                     // ~Printer() reads p.v => heap-use-after-free
};

int main() {
  Doc d;
  d.s.assign(200, 'x');
  d.run();
  return 0;
}
