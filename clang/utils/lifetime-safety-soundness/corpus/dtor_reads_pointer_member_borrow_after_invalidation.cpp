// DESC: a borrow taken through a POINTER MEMBER of `this`, invalidated, then read
// only by a destructor at scope end (an implicit use). The member's seed loan has
// no issuing expression, parameter, or `$this` anchor, and the implicit-use
// branch of the reporter had no fallback, so the detected invalidation emitted
// nothing. The parameter spelling was reported.
// EXPECT-ASAN: heap-use-after-free
#include <string>
#include <string_view>
volatile char sink;
struct [[gsl::Pointer]] Guard {
  explicit Guard(std::string_view v [[clang::lifetimebound]]) : v_(v) {}
  ~Guard() { sink = v_[0]; }
private:
  std::string_view v_;
};
struct [[gsl::Pointer]] S {
  std::string *s;
  void f() {
    Guard g(*s);
    s->append(1000, 'x');
  }
};
int main() {
  std::string str = "hello world, long enough to be on the heap!!";
  S obj{&str};
  obj.f();
}
