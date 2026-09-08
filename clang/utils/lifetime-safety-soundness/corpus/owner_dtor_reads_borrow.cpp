// DESC: a scope guard's destructor reads a borrow the guard was given, and the guard is declared
// FIRST -- so it is destroyed LAST, after the string it borrows. Reverse declaration order makes
// `~ScopedSpan()` read a freed buffer.
//
// The analysis models a non-trivial destructor as a USE of the object precisely so this is
// caught: keeping the borrow live to the destruction point makes the borrowed-from local report
// at its own expiry. Owners were excluded from that, on the grounds that destroying an owner
// frees what it owns rather than dereferencing a borrow into something else.
//
// True of the owner's own storage -- but an owner can ALSO hold a borrow into something it does
// not own, and once a user owner's members became tracked, that borrow is visible. So the
// exclusion went stale, and it went stale for exactly the shape most likely to have a logging
// destructor: a scope guard holding a view of a name.
//
// The line is now whether the owner is LIBRARY-owned. A user owner's members are tracked and its
// own destructor can read them; a library owner's are opaque -- destroying a std::unique_ptr
// destroys its pointee, and whether THAT reads a borrow is the pointee's own destructor's
// business, modelled where the pointee is destroyed.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
#include <vector>
volatile unsigned g_sink;
struct [[gsl::Owner(char)]] ScopedSpan {
  ~ScopedSpan() { emit(); }
  void emit() const { if (!name_.empty()) g_sink = (unsigned)name_[0]; }
  friend void handleRequest(unsigned);
private:
  std::vector<unsigned> samples_;
  std::string_view name_;
};
void handleRequest(unsigned id) {
  ScopedSpan span;                                                 // destroyed LAST
  std::string op = "handle-request-number-" + std::to_string(id);  // destroyed FIRST
  span.name_ = op;
}
int main() { handleRequest(7); return 0; }
