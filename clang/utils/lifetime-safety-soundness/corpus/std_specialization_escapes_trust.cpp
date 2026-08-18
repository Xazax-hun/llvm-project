// DESC: a user specialization of a standard template, which is legal conforming C++ and lands
// literally in namespace `std` -- so it received the trust meant for the library. Eight checks
// asked "is this spelled in namespace std?" when they meant "is this library-owned code?", and
// the namespace alone says nothing about who wrote it.
//
// The control is the whole argument: this file with `::my_hash` instead of `std::hash` is
// reported by -Wlifetime-safety-destruction-order. The namespace was the entire licence, so
// arbitrary user code entered the trust boundary with no trickery at all -- no reserved names,
// no misdeclaration, just the idiomatic way to make a program-defined type hashable.
//
// Four distinct licences were reachable this way: static storage duration with a never-verified
// destructor (here), a verified body calling arbitrary user code through a member of a
// specialization, a container hook needing no promise (which reopened ca0de817239d verbatim by
// spelling the allocator `std::allocator<Key>`), and a class-specific `operator delete` going
// unverified.
//
// The fix asks where the code was WRITTEN: a declaration in a library namespace is trusted only
// when it is also in a system header. An implicit specialization such as `std::vector<int>`
// reports the pattern's location in the library header, so ordinary use keeps its trust.
// EXPECT-ASAN: heap-use-after-free
#include <functional>
#include <string>

volatile char sink;

struct Key {};

extern std::string g;

// A user-written specialization, in namespace std, with a destructor nothing verified.
namespace std {
template <> struct hash<Key> {
  ~hash() { sink = g.data()[0]; }
};
} // namespace std

std::hash<Key> h; // declared first -> destroyed LAST, after g
std::string g = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";

int main() {
  sink = g[1];
  return 0;
}
