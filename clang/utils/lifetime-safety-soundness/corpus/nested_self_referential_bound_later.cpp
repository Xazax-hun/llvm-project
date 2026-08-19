// DESC: the same nested self-referential bind as
// nested_self_referential_view.cpp, established by an ordinary method rather
// than the constructor. Same root cause -- the destination container is the
// nested view member, a sibling of the borrowed owner rather than its enclosing
// object -- and worth pinning separately because nothing about the check is
// specific to constructors, so a fix that only reached member-initializers
// would leave this live.
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
  Header hdr;
  std::string body;

public:
  Message()
      : body("Content-Type: text/html; charset=utf-8 and more padding") {}
  void bind() { hdr.name = body; }
  void reload() {
    body = std::string("X-Other: a completely different long value");
  }
  char first() const { return hdr.name[0]; }
};

int main() {
  Message m;
  m.bind();
  m.reload();
  sink = m.first();
  return 0;
}
