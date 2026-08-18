// DESC: a container constructed on the HEAP inside a verified destructor. The rule that
// catches this asks a question about the type -- does creating it run a constructor this
// body cannot see, which for a container includes its elements' -- and the answer for
// `std::vector<Logger>` was already correct. It was simply never asked here.
//
// The question was wired to two spellings: an automatic local's declaration, and a
// CXXBindTemporaryExpr. A `new` expression is neither, so nothing asked; and because the
// library type's OWN constructor is trusted (it is library code), the per-constructor
// report stayed silent too. The identical `std::vector<Logger> v(1);` one line away was
// refused, which is what localized this to the missing call rather than to the predicate.
//
// The victim shape throughout this family: a class whose CONSTRUCTOR is user code but
// whose DESTRUCTOR is trivial. Every type-level rule asks whether the destructor is safe,
// so `Logger` passes all of them, and `std::vector<Logger>` inherits that pass.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <vector>

volatile char sink;

// Trivially destructible, so no type rule constrains it. Its constructor is the hazard.
struct Logger {
  Logger();
};

struct [[clang::destruction_order_safe]] A {
  ~A();
};

A a;                    // constructed first  -> destroyed LAST
std::string g_name = "a-global-string-long-enough-that-its-buffer-is-heap-allocated";

// By the time this runs at shutdown, `g_name`'s buffer is already freed.
Logger::Logger() { sink = g_name.data()[0]; }

// Allocating the container runs one `Logger` constructor, from inside the library.
A::~A() {
  auto *p = new std::vector<Logger>(1);
  (void)p;
}

int main() { return 0; }
