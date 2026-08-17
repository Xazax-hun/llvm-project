// DESC: an unmodeled construct hidden in a default member initializer reached through
// AGGREGATE initialization. Base-to-derived conversions, `reinterpret_cast`, recovering a
// typed pointer from a `void *`, and union member access are all refused outright, because
// none of them can be modeled. All four were invisible in this one position.
//
// Reached through a constructor -- written or implicit -- a default member initializer belongs
// to that constructor and is analyzed with it. Through aggregate initialization there is no
// constructor at all: the `CXXDefaultInitExpr` sits inline in the enclosing function, and the
// CFG does not descend into its subexpression, so nothing inside is ever handed to a Visit
// method. Moving the identical expression into a statement, or giving the class a constructor
// that leaves the member defaulted, made it fire -- which is what localized the gap to the
// aggregate form rather than to the refusal itself.
//
// The `char` payload is what defeats the remaining backstop: a pointer or view payload is
// caught by -Wlifetime-safety-lost-loan, so the borrow has to be consumed into a scalar here.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct Base {
  virtual ~Base();
};
struct Derived : Base {
  std::string s{"a-derived-member-string-long-enough-to-heap-allocate-for-sure"};
};

// The conversion hides in the default member initializer. `Probe` has no constructor, so
// `Probe p{b}` initializes it as an aggregate.
struct [[gsl::Pointer]] Probe {
  Base *b;
  char v = static_cast<Derived *>(b)->s[0];
};

// Truthful annotation: `b` does not escape.
static char peek(Base *b [[clang::noescape]]) {
  Probe p{b};
  return p.v;
}

// By the time this runs, ~Derived has already freed `s`'s buffer.
Base::~Base() { sink = peek(this); }

int main() {
  Derived d;
  (void)&d;
  return 0;
}
