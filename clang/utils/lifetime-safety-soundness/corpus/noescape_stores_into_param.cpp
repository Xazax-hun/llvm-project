// DESC: a [[clang::noescape]] parameter's borrow stored into storage reached
// through ANOTHER parameter. The `this` spelling of the same body was always
// reported: member origins of the implicit object are seeded at entry, so the
// store lands on the FIELD's origin and function exit emits a field escape the
// noescape verifier consumes. A store through a parameter lands on a transient
// member-access origin owned by no declaration, so no escape fact was emitted
// and nothing checked it -- though `void adopt(sv s) { sv_ = s; }` and
// `static void adopt(Box &b, sv s) { b.sv_ = s; }` mean the same thing. Keeping
// the member private also kept the type clear of the owner-public-borrow rule,
// so a [[gsl::Owner]] could adopt a caller's borrow with no diagnostic at all.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

class [[gsl::Owner]] Box {
  std::string_view sv; // private -> no owner-public-borrow diagnostic

public:
  // `s` is promised not to escape, and is then parked in `b`.
  static void adopt(Box &b [[clang::noescape]],
                    std::string_view s [[clang::noescape]]) {
    b.sv = s;
  }
  char first() const { return sv[0]; }
};

int main() {
  Box b;
  {
    std::string t(60, 'x');
    Box::adopt(b, t);
  }                 // t dies; b still holds a view of its buffer
  sink = b.first(); // heap-use-after-free
  return 0;
}
