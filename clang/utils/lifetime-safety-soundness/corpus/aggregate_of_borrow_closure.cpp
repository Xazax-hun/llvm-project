// DESC: a by-reference-capturing closure is wrapped in a plain user-defined
// aggregate (`Box<F>{closure}`), returned, and used inline. The closure holds a
// borrow of a local std::string; the Box outlives that local. The aggregate's
// ownership was judged borrow-free because isUnknownOwnershipType treats every
// lambda as known-safe (a lambda value is modeled directly), so RecordHoldsBorrow
// never inspected the closure member's captures -> the Box looked origin-free and
// the escaping-temporary / unknown-ownership / lost-loan backstops were all
// bypassed. Found by the multi-agent bypass hunt (Agent A). The fix makes a
// containing record see a closure member that captures a borrow.
// EXPECT-ASAN: stack-use-after-return
#include <string>

template <class F> struct Box {
  F f;
};

__attribute__((noinline)) auto make() {
  std::string s = "this is a long string exceeding the sso buffer length 123456";
  auto c = [&s] { return s[0]; }; // closure borrows the local `s`
  return Box<decltype(c)>{c};     // `s` dangles inside the returned Box
}

int main() {
  return (int)make().f(); // use-after-free: read `s` through the dangling capture
}
