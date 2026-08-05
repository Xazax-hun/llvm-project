// DESC: a destructor body stores a borrow of a LOCAL into a member, and that
// member's own destructor -- which runs *after* the body -- reads it. The premise
// that "by the time a destructor returns the object is gone, so nothing can read
// its members" is false: member destructors (and base destructors) run after the
// body. Because a member's destruction was not modeled at all (the CFG element
// dispatch handled CFGStmt / CFGInitializer / CFGLifetimeEnds / CFGCleanupFunction
// / CFGFullExprCleanup, but not CFGMemberDtor), the borrow was not live at the
// local's expiry and the dangle went unreported. A member's destruction is now a
// use of that member, exactly as a local object's destruction already was.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>

volatile char sink;

struct Inner {
  std::string_view v;
  ~Inner() {
    if (!v.empty())
      sink = v[0]; // runs after ~Outer's body; reads the dead local's buffer
  }
};

struct [[gsl::Owner]] Outer {
  Inner in;
  ~Outer() {
    std::string tmp = "a local string long enough to need a heap buffer ok";
    in.v = tmp; // borrow of a local stored into a member
  }             // tmp dies here; ~Inner runs next
};

int main() {
  { Outer o; }
  return 0;
}
