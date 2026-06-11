// DESC: a [[gsl::Pointer]] view captures a borrow into a local string, and its
// NON-TRIVIAL destructor reads that borrow. Declared before the borrowed-from
// string, the view is destroyed LAST (reverse order), so its destructor reads
// the string's already-freed buffer. The analysis is intra-procedural and never
// sees the out-of-line ~Ref() use the captured view; modeling scope-exit
// destruction of a borrow-holding object as a use closes this. Found by the 4th
// multi-agent bypass hunt (C1).
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

struct [[gsl::Pointer]] Ref {
  std::string_view sv;
  Ref() = default;
  Ref &operator=(std::string_view s [[clang::lifetime_capture_by(this)]]);
  ~Ref();
};
Ref &Ref::operator=(std::string_view s [[clang::lifetime_capture_by(this)]]) {
  sv = s;
  return *this;
}
Ref::~Ref() {
  if (!sv.empty()) {
    volatile char c = sv[0]; // reads the borrowed buffer in the destructor
    (void)c;
  }
}

int main() {
  Ref r;                          // declared first  -> destroyed LAST
  std::string backing(100, 'a');  // declared second -> destroyed FIRST
  r = backing;                    // r captures a view into backing's buffer
  return 0;                       // backing freed, then ~Ref() reads it -> UAF
}
