// DESC: an UNNAMED BIT-FIELD in a [[gsl::Pointer]] aggregate. Aggregate
// initialization is modelled by zipping the initializers against `RD->fields()`, so
// that a reference member can be recognised and bound to the initializer's STORAGE
// rather than to its value.
//
// An unnamed bit-field is in `fields()` but takes no initializer, so the two lists
// drift at the first one and every later initializer is attributed to the wrong
// member. The borrow bound to the reference member was then handled as an ordinary
// value and dropped -- so a [[clang::noescape]] argument escaped into the returned
// object with nothing said, and the caller kept the object across a reallocation.
//
// The same struct without the bit-field, and with a NAMED bit-field (which does take
// an initializer), were both reported all along.
// EXPECT-ASAN: heap-use-after-free
#include <vector>

volatile int g_sink;

struct [[gsl::Pointer(int)]] V {
  const int *p;
  int : 3; // no initializer in the InitListExpr
  const int &r;
};

static V make(const int &x [[clang::lifetimebound]],
              const std::vector<int> &d [[clang::noescape]]) {
  return V{&x, d[0]}; // `r` borrows d[0], escaping despite [[noescape]]
}

int main() {
  std::vector<int> data(100, 7);
  int anchor = 5;
  V v = make(anchor, data);
  data.resize(1000000); // reallocates: v.r now dangles
  g_sink = v.r;
  return 0;
}
