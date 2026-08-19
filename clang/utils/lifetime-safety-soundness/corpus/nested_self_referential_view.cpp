// DESC: a view member bound to a SIBLING owner member of the same object, where
// the view is one level nested. The direct spelling (`name = body`, with `name`
// a view member of the owner) was reported: the self-referential check compares
// the stored borrow against the destination container's loans and asks whether
// the borrow points INTO that container, and for the direct form the container
// is `$this`, a prefix of `$this.body`. Nesting the view inside a member makes
// the container `$this.hdr` while the borrow is `$this.body` -- siblings, so
// neither is a prefix of the other and nothing matched, though both live in one
// object and the store means the same thing. Making the members private also
// kept the type clear of the owner-public-borrow rule.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct [[gsl::Pointer]] Header {
  std::string_view name;
};

struct [[gsl::Owner]] Message {
private:
  Header hdr; // the view is one level nested
  std::string body;

public:
  Message()
      : body("Content-Type: text/html; charset=utf-8 and more padding") {
    hdr.name = body; // binds the view to a sibling owner
  }
  void reload() {
    body = std::string("X-Other: a completely different long value");
  }
  char first() const { return hdr.name[0]; }
};

int main() {
  Message m;
  m.reload();          // frees the buffer hdr.name views
  sink = m.first();    // heap-use-after-free
  return 0;
}
