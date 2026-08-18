// DESC: a lifetime-extended temporary at STATIC storage duration. Binding a
// static-duration reference to a temporary creates a SECOND object of static storage
// duration -- the temporary itself -- and its type need not be the reference's. The walk
// over static-duration variables judged the DECLARED type, `const Base &`, which strips to
// a trivially destructible `Base`, so it stopped there while `~Reader` ran at shutdown and
// was never verified against anything.
//
// Binding to a subobject makes the two types unrelated entirely: `static const int &r =
// Nasty().n;` declares an `int` and creates a `Nasty`. An rvalue reference extends the
// same way.
//
// The model already had the right check -- written inside a verified body the identical
// construct is caught, at the CXXBindTemporaryExpr -- but a namespace-scope initializer
// has no verified body for it to run in. The guard rail that did work pins the mechanism
// exactly: `static const Nasty &r = Nasty();` was always reported, because there the
// referenced type is itself non-trivially destructible and so the declared type sufficed.
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct Base {}; // trivially destructible, and the only type the declaration names

struct Reader : Base {
  ~Reader();
};

// The extended temporary is dynamically initialized FIRST, so it is destroyed LAST.
static const Base &g_keep = Reader();

// Dynamically initialized second, so its heap buffer is freed FIRST.
std::string g_s = "a-global-string-long-enough-that-its-buffer-is-heap-allocated";

// Runs after g_s is gone.
Reader::~Reader() { sink = g_s.data()[0]; }

int main() {
  sink = g_s.data()[0];
  return 0;
}
