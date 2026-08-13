// DESC: a borrow stored by a NAMESPACE-SCOPE dynamic initializer. Such an
// initializer is code, but no entry point reached it: the per-declaration path runs
// when a FUNCTION scope is popped, and TU mode walks callables and the call graph --
// a file-scope VarDecl is neither, and CallGraph does not descend into initializer
// statements. So the store below was never analyzed at all: not refused with a
// lost-loan or unknown-ownership sentinel, simply invisible. It did not even reach
// the "no CFG" bailout report, so nothing surfaced the gap.
//
// The bug is a cross-global destruction-order dangle. Within a TU, initialization
// follows declaration order, so destruction is the reverse: gWatch is constructed
// first and therefore destroyed LAST, reading a view of gBig after gBig's buffer is
// already freed. Writing the identical store in main(), in a constructor body, or in
// a lambda was caught -- only the bare initializer escaped, which also defeated the
// broad -Wview-on-mutable-global ban outright.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Pointer]] Watch {
  std::string_view v;
  ~Watch() {
    if (!v.empty())
      sink = v[0]; // reads gBig's buffer, already freed
  }
};

Watch gWatch;                 // constructed 1st -> destroyed LAST
std::string gBig(200, 'a');   // constructed 2nd -> destroyed FIRST

// The store nothing used to analyze.
static int gWire = (gWatch.v = gBig, 0);

int main() { return 0; }
