// DESC: a '[[clang::destruction_order_safe]]' destructor reading a STATIC data
// member through `this->`. The walk that enforces the promise looked for a
// DeclRefExpr naming an object of static storage duration; `this->text` names the
// same variable but is a MemberExpr, so the walk never saw it. Both the qualified
// `Reader::text` and the unqualified `text` spellings ARE DeclRefExprs and were
// reported, so only the `this->` spelling of one hazard slipped -- and it is the
// hazard the promise exists to rule out: the member's string is destroyed before
// the object that reads it at shutdown.
// FLAGS: -Wno-unused
// EXPECT-ASAN: heap-use-after-free
#include <string>

volatile char sink;

struct [[clang::destruction_order_safe]] Reader {
  static const std::string text;
  ~Reader() { sink = this->text[0]; } // MemberExpr, not DeclRefExpr
};

Reader g_reader; // atexit'd FIRST -> destroyed LAST
const std::string Reader::text =
    "a string long enough to be heap allocated 12345";

int main() { return 0; }
