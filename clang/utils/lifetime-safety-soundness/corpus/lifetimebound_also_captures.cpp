// DESC: a '[[clang::lifetimebound]]' method that ALSO captures the parameter into the object.
// The annotation describes the RETURN VALUE and nothing else, so the store into a member is a
// second relationship the declaration never advertises: the object now aliases the argument,
// and a caller reading only the declaration cannot tell it must keep the argument alive.
//
// Nothing checked it. The lifetimebound claim is satisfied -- the function really does return
// the parameter, truthfully -- and the annotation suppressed the unannotated-indirection
// backstop, so the capture went unchecked and the borrow dangled with no diagnostic anywhere.
//
// What made this worth fixing beyond the hole itself: the model SUGGESTED the annotation that
// creates it. Leaving the parameter unannotated draws an intra-TU suggestion to add
// '[[clang::lifetimebound]]', with a fix-it, on exactly the parameter that gets captured.
//
// The honest spellings both work and are unaffected: a type that holds a borrow is a view, so
// '[[gsl::Pointer]]' plus '[[clang::lifetime_capture_by(this)]]' models the capture and
// reports the dangling use at the CALL SITE; and a genuine owner that stores an owned copy is
// clean.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

class [[gsl::Owner]] Cache {
  std::string_view key;

public:
  std::string_view rekey(const std::string &k [[clang::lifetimebound]]) {
    key = k;  // captured into *this, which the declaration does not advertise
    return k; // lifetimebound: truthful, and the verifier is satisfied
  }
  char peek() const { return key.empty() ? 0 : key[0]; }
};

int main() {
  Cache c;
  {
    std::string tmp(64, 'a');
    c.rekey(tmp);
  } // tmp's buffer is freed here
  sink = c.peek();
  return 0;
}
