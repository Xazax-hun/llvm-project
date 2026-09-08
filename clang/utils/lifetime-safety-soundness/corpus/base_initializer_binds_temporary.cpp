// DESC: a temporary passed to a BASE-CLASS constructor whose parameter is
// [[clang::lifetimebound]]. A base initializer stores into the base subobject
// of `this`, exactly as a member initializer stores into a member -- but only
// the member path flowed the initializer's origins into the destination. The
// base path emitted an escape fact and returned; that fact lets the annotation
// verifier see an escaped PARAMETER loan, but it deposits nothing.
//
// So the borrow rested nowhere. The temporary died with no origin holding it,
// the destructor's read of it was missed, and the identical initializer of a
// MEMBER of the same type was reported all along.
//
// The deposit has to land on the `this` origin, not on the pointee: that is the
// origin a whole-object store resolves to and the one the exit escape reads. On
// the pointee the borrow is still live (the exit use covers both levels) but
// invisible to the annotation checks -- which is what left a lifetimebound
// constructor parameter forwarded to a base reported as "could not verify".
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Pointer]] ScopeLogger {
  std::string_view label;
  explicit ScopeLogger(std::string_view l [[clang::lifetimebound]])
      : label(l) {}
  // Reads the borrow at destruction, after the temporary is long gone.
  ~ScopeLogger() {
    for (unsigned i = 0; i < label.size(); ++i)
      sink = label[i];
  }
};

struct [[gsl::Pointer]] TimedScope : ScopeLogger {
  // The temporary dies at the end of this mem-initializer, but `label` outlives
  // it by the whole body of the object.
  TimedScope()
      : ScopeLogger(
            std::string("timed scope label too long for any SSO buffer")) {}
};

int main() {
  TimedScope ts;
  return 0;
}
