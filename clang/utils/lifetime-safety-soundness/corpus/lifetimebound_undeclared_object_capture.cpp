// DESC: a [[clang::lifetimebound]] parameter that is ALSO captured into the object, through a
// helper that declares the capture honestly. lifetimebound describes the RETURN VALUE and
// nothing else, so a caller keeps the argument alive for the result -- discard the result and
// the object is left holding a dangling borrow. The check keyed on a store into a NAMED member,
// so only the direct `v = p` spelling was caught; landing the borrow on the OBJECT instead was
// silent for every spelling (a helper declared lifetime_capture_by(this), a whole-object
// assignment, a store through a named temporary, a placement new, an inherited setter). Note
// the helper's own annotation is TRUTHFUL: the lie is in `take`, whose declaration admits only
// the return relationship.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Pointer]] H {
  std::string_view v;
  void setD(std::string_view p [[clang::lifetime_capture_by(this)]]) { v = p; }   // honest
  std::string_view take(std::string_view p [[clang::lifetimebound]]) {            // LIE
    setD(p);
    return p;
  }
  void report() const { sink = v[0]; }
};

int main() {
  std::string keep(64, 'k');
  H obj{keep};
  { std::string s(64, 'x'); obj.take(s); }   // return discarded
  obj.report();                              // reads s's freed buffer
  return 0;
}
